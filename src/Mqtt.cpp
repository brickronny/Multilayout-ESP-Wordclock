// Prerequisites for ClockWork.h (pulled in by Mqtt.h), which is not yet
// self-contained: include the types it relies on first, matching the unity
// build's include order.
#include "WordClockState.h"
#include "WordClockTypes/ClockType.hpp"

#include "Mqtt.h"

#include "Config.h"
#include "ImageOverlay.h"
#include "Led.h"
#include "TextOverlay.h"
#include "TimerOverlay.h"
#include "WebPageAdapter.h"
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiClient.h>

#ifdef ESP8266
#include <ESP8266WiFi.h>
#elif defined(ESP32)
#include <WiFi.h>
#endif

// EEPROMAnything.h defines its functions in the header (still part of the unity
// build), so it cannot be included in a second TU without multiple definitions.
// Mqtt only needs eeprom::write(); forward-declare it and let the linker
// resolve to the single definition in the WordClock.cpp translation unit.
namespace eeprom {
void write();
}

extern Led led;
extern WiFiClient client;
extern ClockType *usedClockType;

#define HOMEASSISTANT_DISCOVERY_TOPIC "homeassistant"

#define RETRY_INTERVALL_WITHIN_5_MINUTES 15000 // 15 Seconds
#define MAX_RETRIES_WITHIN_5_MINUTES 20

#define RETRY_INTERVALL 3600000 // 1 Hour

// Static PubSubClient callbacks need a way back to the single Mqtt instance.
Mqtt *mqttInstance = nullptr;

PubSubClient mqttClient(client);

static String topic(const char *suffix) {
    return String(G.mqtt.topic) + "/" + suffix;
}

static void broadcastToWeb(JsonDocument &doc) {
    char buffer[200];
    size_t length = serializeJson(doc, buffer);
    webSocket.broadcastTXT(buffer, length);
}

template <typename T>
static void broadcastToWeb(const char *command, const char *key, T value) {
    StaticJsonDocument<200> doc;
    doc["command"] = command;
    doc[key] = value;
    broadcastToWeb(doc);
}

// Keep inbound payloads bounded; PubSubClient's default buffer is smaller, but
// discovery raises it and callbacks should still avoid variable-length stacks.
static constexpr unsigned int MQTT_MAX_PAYLOAD_LENGTH = 512;
// A text/set message: TEXT_MAX_CHARS glyphs, up to two UTF-8 bytes each for
// the umlauts, JSON escapes and the other keys.
static constexpr unsigned int TEXT_MAX_BYTES = TEXT_MAX_CHARS * 4 + 256;

static constexpr int32_t MQTT_CONNECT_TIMEOUT_MS = 2000;

static const char *mqttFailure = "disconnected";

static const char *failureForState(int state) {
    switch (state) {
    case MQTT_CONNECT_BAD_CREDENTIALS:
        return "bad-credentials";
    case MQTT_CONNECT_UNAUTHORIZED:
        return "unauthorized";
    case MQTT_CONNECT_BAD_CLIENT_ID:
        return "bad-client-id";
    case MQTT_CONNECT_BAD_PROTOCOL:
    case MQTT_CONNECT_UNAVAILABLE:
        return "refused";
    default:
        return "unreachable";
    }
}

static bool connectToBroker() {
    if (client.connected()) {
        return true;
    }

    IPAddress brokerIp;
#ifdef ESP8266
    const int resolved =
        WiFi.hostByName(G.mqtt.serverAdress, brokerIp, MQTT_CONNECT_TIMEOUT_MS);
#else
    const int resolved = WiFi.hostByName(G.mqtt.serverAdress, brokerIp);
#endif
    if (resolved != 1) {
        Serial.println("MQTT: DNS lookup for broker failed");
        mqttFailure = "server-not-found";
        return false;
    }

#ifdef ESP8266
    client.setTimeout(MQTT_CONNECT_TIMEOUT_MS);
    const bool ok = client.connect(brokerIp, G.mqtt.port);
#else
    const bool ok =
        client.connect(brokerIp, G.mqtt.port, MQTT_CONNECT_TIMEOUT_MS);
#endif
    if (!ok) {
        Serial.println("MQTT: Connection to broker failed");
        mqttFailure = "unreachable";
    }
    return ok;
}

// Home Assistant's birth message: HA publishes this when it (re)starts, the cue
// to re-announce discovery and state.
#define HOMEASSISTANT_STATUS_TOPIC HOMEASSISTANT_DISCOVERY_TOPIC "/status"

// Stream a retained JSON document to the broker without a full-payload buffer.
// PubSubClient only needs room for the header and topic, so discovery configs
// can exceed the configured buffer size.
static bool publishJsonStream(const String &topic, const JsonDocument &doc) {
    if (doc.overflowed()) {
        Serial.print("MQTT: Discovery document overflowed for topic ");
        Serial.println(topic);
        return false;
    }
    size_t len = measureJson(doc);
    if (!mqttClient.beginPublish(topic.c_str(), len, true)) {
        Serial.print("MQTT: Failed to begin discovery publish on ");
        Serial.println(topic);
        return false;
    }
    serializeJson(doc, mqttClient);
    return mqttClient.endPublish();
}

static String discoveryTopic(const char *platform, const String &node) {
    return String(HOMEASSISTANT_DISCOVERY_TOPIC) + "/" + platform + "/" + node +
           "/config";
}

static String macDiscoveryNode() {
    String node = WiFi.macAddress();
    node.replace(":", "");
    return node;
}

static void beginDiscovery(JsonDocument &doc, const String &deviceId) {
    doc.clear();
    doc["avty_t"] = topic("availability");
    doc["pl_avail"] = "online";
    doc["pl_not_avail"] = "offline";

    JsonObject device = doc.createNestedObject("dev");
    device.createNestedArray("ids").add(deviceId);
    JsonArray macConnection =
        device.createNestedArray("cns").createNestedArray();
    macConnection.add("mac");
    macConnection.add(deviceId);
    device["name"] = String(G.mqtt.clientId);
    device["sw"] = VERSION;
    device["mdl"] = "Word Clock";
    device["mf"] = "ESPWortuhr";
    device["cu"] = "http://" + WiFi.localIP().toString();

    JsonObject origin = doc.createNestedObject("o");
    origin["name"] = "ESPWortuhr";
    origin["sw"] = VERSION;
    origin["url"] = "https://github.com/ESPWortuhr/Wortuhr";
}

// Publish a diagnostic sensor entity config. Keys are the Home Assistant short
// forms (dev_cla=device_class, etc.). A null argument omits its key. The id
// doubles as the unique_id and discovery node suffix so both stay stable.
static void publishDiagSensor(JsonDocument &doc, const String &deviceId,
                              const String &node, const char *id,
                              const char *name, const char *deviceClass,
                              const char *unit, const char *icon,
                              const char *stateClass,
                              const char *valueTemplate) {
    beginDiscovery(doc, deviceId);
    doc["uniq_id"] = deviceId + "_" + id;
    doc["name"] = name;
    if (deviceClass)
        doc["dev_cla"] = deviceClass;
    if (unit)
        doc["unit_of_meas"] = unit;
    if (icon)
        doc["ic"] = icon;
    if (stateClass)
        doc["stat_cla"] = stateClass;
    doc["ent_cat"] = "diagnostic";
    doc["stat_t"] = topic("diagnostics");
    doc["val_tpl"] = valueTemplate;
    publishJsonStream(discoveryTopic("sensor", node + "_" + id), doc);
}

// Clear a retained discovery config that this firmware no longer publishes, so
// it does not leave stale or duplicate entities in Home Assistant.
static void clearDiscovery(const char *platform, const String &node) {
    mqttClient.publish(discoveryTopic(platform, node).c_str(), "", true);
}

static constexpr uint32_t DISCOVERY_MIGRATION_DELAY_MS = 1000;
static bool discoveryMigrated = false;
static bool discoveryMigrationPending = false;
static uint32_t discoveryMigrationStartedAt = 0;

static void clearRetiredDiscovery() {
    String node = macDiscoveryNode();
    clearDiscovery("device", node);
    clearDiscovery("light", String(G.mqtt.topic) + "/light");
    clearDiscovery("sensor", node + "_adc_value");
    clearDiscovery("sensor", node + "_adc_raw");
}

// Home Assistant "select" entities exchange human-readable labels, while the
// firmware stores small integers. These tables map between the two so no Jinja
// templates are needed in discovery. Special-event transitions (Birthday / New
// Year, enum 97-99) are intentionally omitted: they auto-trigger by date.
struct LabeledValue {
    const char *label;
    uint8_t value;
};

#define LABELED_VALUE_COUNT(table) (sizeof(table) / sizeof((table)[0]))

static const LabeledValue TRANSITION_TYPES[] = {
    {"Off", 0},         {"Roll Up", 1}, {"Roll Down", 2}, {"Shift Left", 3},
    {"Shift Right", 4}, {"Fade", 5},    {"Laser", 6},     {"Matrix Rain", 7},
    {"Balls", 8},       {"Fire", 9},    {"Snake", 10},    {"Random", 11}};

static const LabeledValue TRANSITION_COLORIZE[] = {
    {"Off", 0}, {"Gradient", 1}, {"Random", 2}};

static const LabeledValue TRANSITION_DURATION[] = {
    {"Short", 1}, {"Medium", 2}, {"Long", 3}};

// Return the label for a stored value, or fallback if the value is unknown.
static const char *labelForValue(const LabeledValue *table, size_t count,
                                 uint8_t value, const char *fallback) {
    for (size_t i = 0; i < count; i++) {
        if (table[i].value == value)
            return table[i].label;
    }
    return fallback;
}

// Resolve an incoming label to its stored value. Returns false (and leaves out
// untouched) when the label is not in the table, so callers can ignore it.
static bool valueForLabel(const LabeledValue *table, size_t count,
                          const char *label, uint8_t &out) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(table[i].label, label) == 0) {
            out = table[i].value;
            return true;
        }
    }
    return false;
}

// Publish a "select" entity config (options list with label<->value mapping)
// as a config entity.
static void publishSelect(JsonDocument &doc, const String &deviceId,
                          const String &node, const char *id, const char *name,
                          const char *icon, const LabeledValue *table,
                          size_t count) {
    beginDiscovery(doc, deviceId);
    doc["uniq_id"] = deviceId + "_" + id;
    doc["name"] = name;
    if (icon)
        doc["ic"] = icon;
    doc["ent_cat"] = "config";
    doc["stat_t"] = topic(id) + "/state";
    doc["cmd_t"] = topic(id) + "/set";
    JsonArray options = doc.createNestedArray("options");
    for (size_t i = 0; i < count; i++)
        options.add(table[i].label);
    publishJsonStream(discoveryTopic("select", node + "_" + id), doc);
}

// Register the instance used by PubSubClient's static callback hook.
Mqtt::Mqtt(ClockWork &cw) : clockWork(cw) { mqttInstance = this; }

Mqtt::~Mqtt() {
    if (mqttInstance == this) {
        mqttInstance = nullptr;
    }
}

//------------------------------------------------------------------------------
// Helper Functions
//------------------------------------------------------------------------------

/* Description:

This function processes the Home Assistant JSON light "state" key. Only valid
ON/OFF values are applied and echoed back; malformed values are ignored.

Input:

const JsonDocument &doc: The JSON document containing the "state" key.

Output:

None
*/

bool Mqtt::processState(const JsonDocument &doc) {
    if (doc.containsKey("state")) {
        const char *state = doc["state"] | "";
        bool stateChanged = false;
        if (!strcmp(state, "ON")) {
            Serial.println("MQTT: Turning ON");
            led.setState(true);
            stateChanged = true;
        } else if (!strcmp(state, "OFF")) {
            Serial.println("MQTT: Turning OFF");
            led.setState(false);
            stateChanged = true;
        }
        if (stateChanged) {
            webStateChanged = true;
        }
        return stateChanged;
    }
    return false;
}

//------------------------------------------------------------------------------

/* Description:

This function processes the Home Assistant JSON light "effect" key. Unknown
effect names are ignored so stale or malformed MQTT commands do not change the
clock mode.

Input:

const JsonDocument &doc: The JSON document containing the "effect" key.

Output:

None
*/

bool Mqtt::processEffect(const JsonDocument &doc) {
    if (doc.containsKey("effect")) {
        const char *effect = doc["effect"] | "";
        bool effectChanged = false;

        if (!strcmp("Wordclock", effect)) {
            G.prog = COMMAND_MODE_WORD_CLOCK;
            effectChanged = true;
        } else if (!strcmp("Seconds", effect)) {
            G.prog = COMMAND_MODE_SECONDS;
            effectChanged = true;
        } else if (!strcmp("Digitalclock", effect)) {
            G.prog = COMMAND_MODE_DIGITAL_CLOCK;
            effectChanged = true;
        } else if (!strcmp("Scrollingtext", effect)) {
            G.prog = COMMAND_MODE_SCROLLINGTEXT;
            effectChanged = true;
        } else if (!strcmp("Rainbowcycle", effect)) {
            G.prog = COMMAND_MODE_RAINBOWCYCLE;
            effectChanged = true;
        } else if (!strcmp("Rainbow", effect)) {
            G.prog = COMMAND_MODE_RAINBOW;
            effectChanged = true;
        } else if (!strcmp("Fire", effect)) {
            G.prog = COMMAND_MODE_FIRE;
            effectChanged = true;
        } else if (!strcmp("Color", effect)) {
            G.prog = COMMAND_MODE_COLOR;
            effectChanged = true;
        } else if (!strcmp("Symbol", effect)) {
            G.prog = COMMAND_MODE_SYMBOL;
            effectChanged = true;
        }

        if (effectChanged) {
            G.progInit = true;
            parametersChanged = true;
            webStateChanged = true;
        }
        return effectChanged;
    }
    return false;
}

//------------------------------------------------------------------------------

const char *Mqtt::getEffectName() {
    if (isWordClockMode(G.prog)) {
        return "Wordclock";
    }
    switch (G.prog) {
    case COMMAND_MODE_SECONDS:
        return "Seconds";
    case COMMAND_MODE_DIGITAL_CLOCK:
        return "Digitalclock";
    case COMMAND_MODE_SCROLLINGTEXT:
        return "Scrollingtext";
    case COMMAND_MODE_RAINBOWCYCLE:
        return "Rainbowcycle";
    case COMMAND_MODE_RAINBOW:
        return "Rainbow";
    case COMMAND_MODE_FIRE:
        return "Fire";
    case COMMAND_MODE_COLOR:
        return "Color";
    case COMMAND_MODE_SYMBOL:
        return "Symbol";
    default:
        return nullptr;
    }
}

//------------------------------------------------------------------------------

/* Description:

This function processes the "scrolling_text" key. The text is always copied with
the destination buffer size because Home Assistant text entities can send longer
payloads than the clock can store.

Input:

const JsonDocument &doc: The JSON document containing the "scrolling_text" key.

Output:

None
*/

void Mqtt::processScrollingText(const JsonDocument &doc) {
    if (doc.containsKey("scrolling_text")) {
        strlcpy(G.scrollingText, doc["scrolling_text"] | "",
                sizeof(G.scrollingText));

        broadcastToWeb("scrolltext", "scrolling_text", G.scrollingText);
    }
}

//------------------------------------------------------------------------------

/* Description:

This function processes the Home Assistant JSON light "color" key. Color changes
are applied only when both HS components are present; brightness is left
unchanged.

Input:

const JsonDocument &doc: The JSON document containing the "color" key.

Output:

None
*/

bool Mqtt::processColor(const JsonDocument &doc) {
    JsonObjectConst color = doc["color"];
    if (!color.isNull() && color.containsKey("h") && color.containsKey("s")) {
        // Convert values from Home Assistant (0-360 for Hue, 0-100 for
        // Saturation) to clock format (0-1 for both)
        float h = constrain(color["h"].as<float>(), 0, 360) / 360.0f;
        float s = constrain(color["s"].as<float>(), 0, 100) / 100.0f;

        // Keep current brightness
        G.color[Foreground] = HsbColor(h, s, G.color[Foreground].B);
        parametersChanged = true;
        webStateChanged = true;
        return true;
    }
    return false;
}

//------------------------------------------------------------------------------

/* Description:

This function processes the Home Assistant JSON light "brightness" key. Values
are clamped to the 0-255 MQTT light range before being normalized to the clock's
0.0-1.0 brightness value.

Input:

const JsonDocument &doc: The JSON document containing the "brightness" key.

Output:

None
*/

bool Mqtt::processBrightness(const JsonDocument &doc) {
    if (doc.containsKey("brightness")) {
        // Manual brightness commands must not fight the ambient-light loop.
        if (G.autoBrightEnabled) {
            Serial.println("MQTT: Ignoring brightness change - auto brightness "
                           "is enabled");
            return false;
        }

        int rawBrightness = constrain(doc["brightness"] | 0, 0, 255);
        float brightness = float(rawBrightness) / 255.0f;
        brightness = max(0.0f, min(1.0f, brightness));

        // Set brightness directly
        G.color[Foreground] =
            HsbColor(G.color[Foreground].H, G.color[Foreground].S, brightness);
        G.color[GradientEnd].B = brightness;
        Serial.print("MQTT: Setting manual brightness: ");
        Serial.println(brightness);

        parametersChanged = true;
        webStateChanged = true;
        return true;
    }
    return false;
}

//------------------------------------------------------------------------------

/* Description:

This function checks if a character array representing an MQTT user is empty. An
MQTT user is considered empty if it contains only null characters ('\0') up to
the specified length.

Input:

None
Output:

true if the MQTT user array is empty.
false if the MQTT user array is not empty.
*/

/* static */ bool Mqtt::checkIfMqttUserIsEmpty() {
    for (uint8_t i = 0; i < PAYLOAD_LENGTH; i++) {
        if (G.mqtt.user[i] != '\0' && !isSpace(G.mqtt.user[i])) {
            return false; // Array is not empty
        }
    }
    return true; // Array is empty
}

//------------------------------------------------------------------------------

/* Description:

This function initializes an MQTT client connection on an ESP8266 device using
the PubSubClient library. It sets up the MQTT client with the provided MQTT
server address, port, client ID, and other optional parameters such as user
credentials and topic subscription.

Input:

None
Output:

None
*/

void Mqtt::init() {
    mqttClient.setServer(G.mqtt.serverAdress, G.mqtt.port);
    mqttClient.setCallback(callback);
#if IMAGE_MAX_FRAMES > 0
    // Pictures and animations arrive in one message (image/set).
    mqttClient.setBufferSize(
        max<unsigned int>(IMAGE_MAX_BYTES, TEXT_MAX_BYTES) + 256);
#else
    mqttClient.setBufferSize(TEXT_MAX_BYTES + 256);
#endif

    // Configure LWT (Last Will and Testament)
    String availabilityTopic = topic("availability");

    if (!isConnected()) {
        if (!connectToBroker()) {
            return;
        }

        bool connected;
        if (checkIfMqttUserIsEmpty()) {
            connected =
                mqttClient.connect(G.mqtt.clientId, availabilityTopic.c_str(),
                                   0,          // QoS
                                   true,       // retain
                                   "offline"); // Last Will Message
        } else {
            connected =
                mqttClient.connect(G.mqtt.clientId, G.mqtt.user,
                                   G.mqtt.password, availabilityTopic.c_str(),
                                   0,          // QoS
                                   true,       // retain
                                   "offline"); // Last Will Message
        }
        if (!connected) {
            Serial.print("MQTT: Broker refused connection, state ");
            Serial.println(mqttClient.state());
            mqttFailure = failureForState(mqttClient.state());
            return;
        }
        mqttFailure = "disconnected";
    }

    // Send online status immediately after connection
    mqttClient.publish(availabilityTopic.c_str(), "online", true);

    // Main control
    mqttClient.subscribe(topic("cmd").c_str());

    // Re-announce discovery and state when Home Assistant (re)starts.
    mqttClient.subscribe(HOMEASSISTANT_STATUS_TOPIC);

    // Additional Topics
    mqttClient.subscribe(topic("scrolltext/set").c_str());
    mqttClient.subscribe(topic("effect_speed/set").c_str());
    mqttClient.subscribe(topic("auto_brightness/set").c_str());
#if IMAGE_MAX_FRAMES > 0
    mqttClient.subscribe(topic("image/set").c_str());
    mqttClient.subscribe(topic("image/clear").c_str());
#endif
    mqttClient.subscribe(topic("timer/set").c_str());
    mqttClient.subscribe(topic("timer/clear").c_str());
    mqttClient.subscribe(topic("text/set").c_str());
    mqttClient.subscribe(topic("text/clear").c_str());

    // Transition settings
    mqttClient.subscribe(topic("transition_type/set").c_str());
    mqttClient.subscribe(topic("transition_colorize/set").c_str());
    mqttClient.subscribe(topic("transition_duration/set").c_str());

    Serial.println("MQTT Connected");
#if IMAGE_MAX_FRAMES > 0
    publishImageInfo();
    publishImageState();
#endif
    publishTimerState();
    publishTextState();
    sendDiscovery(); // Re-announce entities so they self-heal after a
                     // broker restart or purge of retained config
    sendState();     // Send initial state
}

//------------------------------------------------------------------------------

/* Description:

This function attempts to reconnect the MQTT client to the broker after a loss
of connection. It includes a retry mechanism with a maximum number of retries
and a timeout duration.

Input:

None
Output:

None
*/

void Mqtt::reInit() {
    static uint8_t retryCount = 0;
    static uint32_t lastRetryTime = 0;
    static uint32_t retryIntervall = RETRY_INTERVALL_WITHIN_5_MINUTES;

    if (millis() - lastRetryTime >= retryIntervall) {
        retryCount++;
        Serial.print("Reconnecting to MQTT Server. Try ");
        Serial.println(retryCount);
        lastRetryTime = millis();

        init();

        if (retryCount >= MAX_RETRIES_WITHIN_5_MINUTES) {
            Serial.println("Switched to hourly MQTT connect retry");
            retryIntervall = RETRY_INTERVALL;
            retryCount = 0;
        }

        if (isConnected()) {
            retryCount = 0;
            retryIntervall = RETRY_INTERVALL_WITHIN_5_MINUTES;
        }
    }
}

//------------------------------------------------------------------------------

/* Description:

This function checks whether the MQTT client is currently connected to the MQTT
broker. It returns a boolean value indicating the connection status.

Input:

None
Output:

Boolean value:
true if the MQTT client is connected to the broker.
false if the MQTT client is not connected to the broker.
 */

bool Mqtt::isConnected() { return mqttClient.connected(); }

const char *Mqtt::status() {
    if (!G.mqtt.state || G.mqtt.serverAdress[0] == '\0') {
        return "not-configured";
    }
    if (isConnected()) {
        return "connected";
    }
    return mqttFailure;
}

void Mqtt::notifyStatus() {
    static const char *lastStatus = nullptr;
    const char *current = status();
    if (current == lastStatus) {
        return;
    }
    lastStatus = current;
    broadcastToWeb("mqtt-status", "status", current);
}

//------------------------------------------------------------------------------

/* Description:

This function is responsible for managing the MQTT client's main loop. It checks
the MQTT connection status and reinitializes the connection if necessary.
Additionally, it invokes the loop() function of the underlying MQTT client
library to handle incoming MQTT messages and maintain the connection.

Input:

None
Output:

None
*/

void Mqtt::loop() {
    static uint32_t lastStateUpdate = 0;
    const uint32_t STATE_UPDATE_INTERVAL = 30000; // Alle 30 Sekunden

    if (!isConnected()) {
        reInit();
    }
    mqttClient.loop();

    if (discoveryMigrationPending && isConnected() &&
        millis() - discoveryMigrationStartedAt >=
            DISCOVERY_MIGRATION_DELAY_MS) {
        discoveryMigrationPending = false;
        publishDiscoveryConfigs();
        clearRetiredDiscovery();
        discoveryMigrated = true;
    }

    // Regular status update
    if (millis() - lastStateUpdate >= STATE_UPDATE_INTERVAL) {
        sendState();
        lastStateUpdate = millis();
    }

    notifyStatus();
}

//------------------------------------------------------------------------------

/* Description:

This function handles incoming MQTT messages received from the broker. It
parses the payload as JSON data and updates the device's state and
parameters accordingly.

Input:

char *topic: A pointer to a character array containing the topic of the
received message. byte *payload: A pointer to an array of bytes containing
the payload of the received message. unsigned int length: The length of the
payload in bytes. Output:

None
*/

// Apply a label-based select command to a uint8_t setting, persist it, force a
// transition re-init, and always echo the resulting state label back. Unknown
// labels leave the setting unchanged but still echo the current state.
static void applyTransitionSelect(const char *id, const char *msg,
                                  const LabeledValue *table, size_t count,
                                  uint8_t &field) {
    uint8_t value;
    if (valueForLabel(table, count, msg, value)) {
        field = value;
        G.progInit = true;
        eeprom::write();
        webStateChanged = true;
    } else {
        Serial.print("MQTT: Unknown option for ");
        Serial.print(id);
        Serial.print(": ");
        Serial.println(msg);
    }
    mqttClient.publish((topic(id) + "/state").c_str(),
                       labelForValue(table, count, field, table[0].label),
                       true);
}

void Mqtt::callback(char *receivedTopic, byte *payload, unsigned int length) {
    // Determine message type before parsing; not every topic uses JSON.
    String topicStr = String(receivedTopic);

#if IMAGE_MAX_FRAMES > 0
    // Pictures may be far larger than every other message; handled before
    // the fixed-size copy below.
    if (topicStr == topic("image/set")) {
        processImage(payload, length);
        return;
    }
    if (topicStr == topic("image/clear")) {
        if (imageOverlay) {
            // Ends at the next update, so ClockWork hands the display back.
            imageOverlay->stop();
        }
        return;
    }
#endif

    // Messages may be longer than the fixed-size copy below allows.
    if (topicStr == topic("text/set")) {
        processText(payload, length);
        return;
    }
    if (topicStr == topic("text/clear")) {
        textOverlay.stop(); // ClockWork hands the display back
        return;
    }

    // Copy into a fixed buffer so hostile or malformed MQTT payloads cannot
    // allocate arbitrarily large stack frames.
    if (length >= MQTT_MAX_PAYLOAD_LENGTH) {
        Serial.print("MQTT: Ignoring oversized payload on topic ");
        Serial.println(topicStr);
        return;
    }

    char msg[MQTT_MAX_PAYLOAD_LENGTH];
    memcpy(msg, payload, length);
    msg[length] = '\0';

    if (topicStr == topic("timer/set")) {
        processTimer(msg);
        return;
    }
    if (topicStr == topic("timer/clear")) {
        timerOverlay.stop(); // ClockWork hands the display back
        return;
    }

    // Home Assistant's birth message: re-announce discovery and state so the
    // device repopulates after an HA restart, even if retained config was lost.
    if (topicStr == HOMEASSISTANT_STATUS_TOPIC) {
        if (strcmp(msg, "online") == 0 && mqttInstance) {
            Serial.println("MQTT: Home Assistant online, re-announcing");
            mqttInstance->sendDiscovery();
            mqttInstance->sendState();
        }
        return;
    }

    // MQTT switch commands are plain ON/OFF payloads, not JSON.
    if (topicStr == topic("auto_brightness/set")) {
        bool autoBrightnessChanged = false;
        if (strcmp(msg, "ON") == 0) {
            G.autoBrightEnabled = 1;
            Serial.println("MQTT: Auto Brightness ON");
            autoBrightnessChanged = true;
        } else if (strcmp(msg, "OFF") == 0) {
            G.autoBrightEnabled = 0;
            Serial.println("MQTT: Auto Brightness OFF");
            autoBrightnessChanged = true;
        } else {
            Serial.print("MQTT: Unknown command: ");
            Serial.println(msg);
        }

        if (autoBrightnessChanged) {
            parametersChanged = true;
        }

        // Always echo the current retained state, even for invalid commands.
        mqttClient.publish(topic("auto_brightness/state").c_str(),
                           G.autoBrightEnabled ? "ON" : "OFF", true);

        broadcastToWeb("autobright", "value", G.autoBrightEnabled);
        return;
    }

    // Transition selects/number/switch are plain payloads, not JSON. Each
    // applies the setting, persists it, re-inits so it takes effect, and echoes
    // the resulting state back to Home Assistant.
    if (topicStr == topic("transition_type/set")) {
        applyTransitionSelect("transition_type", msg, TRANSITION_TYPES,
                              LABELED_VALUE_COUNT(TRANSITION_TYPES),
                              G.transitionType);
        return;
    }
    if (topicStr == topic("transition_colorize/set")) {
        applyTransitionSelect("transition_colorize", msg, TRANSITION_COLORIZE,
                              LABELED_VALUE_COUNT(TRANSITION_COLORIZE),
                              G.colorize);
        return;
    }
    if (topicStr == topic("transition_duration/set")) {
        applyTransitionSelect("transition_duration", msg, TRANSITION_DURATION,
                              LABELED_VALUE_COUNT(TRANSITION_DURATION),
                              G.transitionDuration);
        return;
    }
    // Remaining command topics use JSON payloads.
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, msg);
    if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.c_str());
        return;
    }

    // Process remaining messages
    if (topicStr == topic("cmd")) {
        bool stateChanged = processState(doc);
        stateChanged |= processEffect(doc);
        processScrollingText(doc);
        stateChanged |= processColor(doc);
        stateChanged |= processBrightness(doc);
        if (stateChanged && mqttInstance) {
            mqttInstance->sendState();
        }
    } else if (topicStr == topic("scrolltext/set")) {
        processScrollingText(doc);
    } else if (topicStr == topic("effect_speed/set")) {
        // Process direct string value
        int speed = atoi(msg);
        if (speed >= EFFECT_SPEED_MIN && speed <= EFFECT_SPEED_MAX) {
            G.effectSpeed = speed;

            // Effect speed is a user setting, so persist accepted values.
            eeprom::write();
            parametersChanged = true;

            // If we are in scrolltext mode, reinitialize the program
            if (G.prog == COMMAND_MODE_SCROLLINGTEXT) {
                G.progInit = true;
            }

            // Send new state to Home Assistant
            mqttClient.publish(topic("effect_speed/state").c_str(),
                               String(G.effectSpeed).c_str(), true);

            webStateChanged = true;
        }
    }
}

//------------------------------------------------------------------------------

/* Description:

This function is responsible for publishing the current state of the device
to an MQTT topic. It constructs a JSON message containing information about
the device state, such as the power state (ON or OFF), color settings, and
brightness. The constructed JSON message is then published to the MQTT
broker on a specified topic.

Input:

None
Output:

None
*/

void Mqtt::sendState() {
    // Main status
    {
        StaticJsonDocument<200> doc;
        doc["state"] = (led.getState()) ? "ON" : "OFF";

        // Calculate actual brightness value
        float actualBrightness;
        if (G.autoBrightEnabled) {
            actualBrightness = (ledGain / 100.0f) * G.color[Foreground].B;
        } else {
            actualBrightness = G.color[Foreground].B;
        }
        doc["brightness"] = round(actualBrightness * 255);
        doc["color_mode"] = "hs";

        // Color as object with h and s
        JsonObject color = doc.createNestedObject("color");
        color["h"] = round(G.color[Foreground].H * 360); // Hue 0-360
        color["s"] = round(G.color[Foreground].S * 100); // Saturation 0-100

        const char *effect = getEffectName();
        if (effect) {
            doc["effect"] = effect;
        }

        char buffer[200];
        serializeJson(doc, buffer);
        mqttClient.publish(topic("status").c_str(), buffer, true);
    }

    // Send diagnostic values
    {
        StaticJsonDocument<320> doc;
        doc["lux"] = round(clockWork.getLuxValue()); // Round lux value
        doc["led_gain"] = round(ledGain); // Convert to percent and round
        doc["auto_brightness_source"] =
            autoBrightUsingBH1750 ? "BH1750" : "LDR";
        doc["rssi"] = WiFi.RSSI();             // WiFi signal strength
        doc["ip"] = WiFi.localIP().toString(); // Current IP address
        doc["uptime"] = millis() / 1000;       // Seconds since boot
        char buffer[320];
        serializeJson(doc, buffer);
        mqttClient.publish(topic("diagnostics").c_str(), buffer, true);
    }

    // Effect speed status - Always update current value
    {
        mqttClient.publish(topic("effect_speed/state").c_str(),
                           String(G.effectSpeed).c_str(), true);
    }

    // Scrolling text status
    {
        StaticJsonDocument<200> doc;
        doc["scrolling_text"] = G.scrollingText;
        char buffer[200];
        serializeJson(doc, buffer);
        mqttClient.publish(topic("scrolltext/state").c_str(), buffer, true);
    }

    // Auto brightness status
    {
        mqttClient.publish(topic("auto_brightness/state").c_str(),
                           G.autoBrightEnabled ? "ON" : "OFF", true);
    }

    // Transition settings status (selects echo their labels)
    {
        mqttClient.publish(topic("transition_type/state").c_str(),
                           labelForValue(TRANSITION_TYPES,
                                         LABELED_VALUE_COUNT(TRANSITION_TYPES),
                                         G.transitionType,
                                         TRANSITION_TYPES[0].label),
                           true);
        mqttClient.publish(
            topic("transition_colorize/state").c_str(),
            labelForValue(TRANSITION_COLORIZE,
                          LABELED_VALUE_COUNT(TRANSITION_COLORIZE), G.colorize,
                          TRANSITION_COLORIZE[0].label),
            true);
        mqttClient.publish(
            topic("transition_duration/state").c_str(),
            labelForValue(TRANSITION_DURATION,
                          LABELED_VALUE_COUNT(TRANSITION_DURATION),
                          G.transitionDuration, TRANSITION_DURATION[1].label),
            true);
    }

    // Update online status
    mqttClient.publish(topic("availability").c_str(), "online", true);
}

//------------------------------------------------------------------------------

/* Description:

This function publishes MQTT discovery messages for Home Assistant,
providing configuration details for a light entity. It constructs a JSON
payload according to Home Assistant's MQTT discovery format and
publishes it to the appropriate topic.

Input:

None
Output:

None
*/

void Mqtt::sendDiscovery() {
    if (discoveryMigrated) {
        publishDiscoveryConfigs();
        return;
    }
    mqttClient.publish(discoveryTopic("device", macDiscoveryNode()).c_str(),
                       "{\"migrate_discovery\":true}");
    discoveryMigrationPending = true;
    discoveryMigrationStartedAt = millis();
}

void Mqtt::publishDiscoveryConfigs() {
    // The MAC is the stable device identifier and the base for every entity's
    // unique_id, so renaming the MQTT base topic never orphans the device. The
    // colon-free variant is used in the discovery topic path.
    String unique_id = WiFi.macAddress();
    String discoveryNode = macDiscoveryNode();

    String base = String(G.mqtt.topic);
    DynamicJsonDocument doc(3072);

    // Light: the device's primary entity. name=null makes it inherit the device
    // name instead of repeating it (Home Assistant has_entity_name convention).
    beginDiscovery(doc, unique_id);
    doc["uniq_id"] = unique_id;
    doc["name"] = (const char *)nullptr;
    doc["schema"] = "json";
    doc["stat_t"] = base + "/status";
    doc["cmd_t"] = base + "/cmd";
    doc["brightness"] = true;
    doc["bri_scl"] = 255;
    doc.createNestedArray("sup_clrm").add("hs");
    doc["optimistic"] = false;
    doc["effect"] = true;
    JsonArray effectList = doc.createNestedArray("fx_list");
    effectList.add("Wordclock");
    effectList.add("Seconds");
    effectList.add("Digitalclock");
    effectList.add("Scrollingtext");
    effectList.add("Rainbowcycle");
    effectList.add("Rainbow");
    effectList.add("Fire");
    effectList.add("Color");
    effectList.add("Symbol");
    publishJsonStream(discoveryTopic("light", discoveryNode), doc);

    // Text entity for the scrolling text mode.
    beginDiscovery(doc, unique_id);
    doc["uniq_id"] = unique_id + "_scrolltext";
    doc["name"] = "Scrolling Text";
    doc["ic"] = "mdi:text";
    doc["stat_t"] = base + "/scrolltext/state";
    doc["cmd_t"] = base + "/scrolltext/set";
    doc["val_tpl"] = "{{ value_json.scrolling_text }}";
    doc["cmd_tpl"] = "{ \"scrolling_text\": {{ value | to_json }} }";
    doc["max"] = sizeof(G.scrollingText) - 1;
    publishJsonStream(discoveryTopic("text", discoveryNode + "_scrolltext"),
                      doc);

    // Notify entity: pushes a message to the marquee. It targets the JSON
    // command topic with both the text and the Scrollingtext effect, so the
    // clock switches to scrolling-text mode and shows the message immediately.
    beginDiscovery(doc, unique_id);
    doc["uniq_id"] = unique_id + "_message";
    doc["name"] = "Message";
    doc["ic"] = "mdi:message-text";
    doc["cmd_t"] = base + "/cmd";
    doc["cmd_tpl"] = "{\"scrolling_text\": {{ value | to_json }}, \"effect\": "
                     "\"Scrollingtext\"}";
    publishJsonStream(discoveryTopic("notify", discoveryNode + "_message"),
                      doc);

    // Notify entity for a one-off message: scrolls through once over
    // text/set and returns to the clock, the mode stays as it is.
    beginDiscovery(doc, unique_id);
    doc["uniq_id"] = unique_id + "_message_once";
    doc["name"] = "Message (once)";
    doc["ic"] = "mdi:message-arrow-right";
    doc["cmd_t"] = base + "/text/set";
    doc["cmd_tpl"] = "{\"text\": {{ value | to_json }}}";
    publishJsonStream(discoveryTopic("notify", discoveryNode + "_message_once"),
                      doc);

    // Number entity for animation/effect speed.
    beginDiscovery(doc, unique_id);
    doc["uniq_id"] = unique_id + "_effect_speed";
    doc["name"] = "Effect Speed";
    doc["ic"] = "mdi:speedometer";
    doc["stat_t"] = base + "/effect_speed/state";
    doc["cmd_t"] = base + "/effect_speed/set";
    doc["min"] = 1;
    doc["max"] = 10;
    doc["step"] = 1;
    publishJsonStream(discoveryTopic("number", discoveryNode + "_effect_speed"),
                      doc);

    // Switch entity for ambient-light based brightness control.
    beginDiscovery(doc, unique_id);
    doc["uniq_id"] = unique_id + "_auto_brightness";
    doc["name"] = "Auto Brightness";
    doc["ic"] = "mdi:brightness-auto";
    doc["dev_cla"] = "switch";
    doc["stat_t"] = base + "/auto_brightness/state";
    doc["cmd_t"] = base + "/auto_brightness/set";
    doc["pl_on"] = "ON";
    doc["pl_off"] = "OFF";
    publishJsonStream(
        discoveryTopic("switch", discoveryNode + "_auto_brightness"), doc);

    // Transition settings.
    publishSelect(doc, unique_id, discoveryNode, "transition_type",
                  "Transition Type", "mdi:transition", TRANSITION_TYPES,
                  LABELED_VALUE_COUNT(TRANSITION_TYPES));
    publishSelect(doc, unique_id, discoveryNode, "transition_colorize",
                  "Transition Colorize", "mdi:palette", TRANSITION_COLORIZE,
                  LABELED_VALUE_COUNT(TRANSITION_COLORIZE));
    publishSelect(doc, unique_id, discoveryNode, "transition_duration",
                  "Transition Duration", "mdi:timer-outline",
                  TRANSITION_DURATION,
                  LABELED_VALUE_COUNT(TRANSITION_DURATION));

    // Diagnostic sensors, all read from the shared diagnostics topic.
    publishDiagSensor(doc, unique_id, discoveryNode, "lux", "Illuminance",
                      "illuminance", "lx", "mdi:white-balance-sunny",
                      "measurement", "{{ value_json.lux }}");
    publishDiagSensor(doc, unique_id, discoveryNode, "led_gain", "LED Gain",
                      "power_factor", "%", "mdi:brightness-percent",
                      "measurement", "{{ value_json.led_gain }}");
    publishDiagSensor(doc, unique_id, discoveryNode, "auto_brightness_source",
                      "Auto Brightness Source", nullptr, nullptr,
                      "mdi:lightbulb-auto", nullptr,
                      "{{ value_json.auto_brightness_source }}");
    publishDiagSensor(doc, unique_id, discoveryNode, "rssi", "WiFi Signal",
                      "signal_strength", "dBm", nullptr, "measurement",
                      "{{ value_json.rssi }}");
    publishDiagSensor(doc, unique_id, discoveryNode, "ip", "IP Address",
                      nullptr, nullptr, "mdi:ip-network", nullptr,
                      "{{ value_json.ip }}");
    publishDiagSensor(doc, unique_id, discoveryNode, "uptime", "Uptime",
                      "duration", "s", nullptr, "total_increasing",
                      "{{ value_json.uptime }}");

    // Make the device available now that its entities are announced.
    mqttClient.publish(topic("availability").c_str(), "online", true);
}

//------------------------------------------------------------------------------
// Pictures and animations (image/set, image/clear, image/info, image/state)
//------------------------------------------------------------------------------

#if IMAGE_MAX_FRAMES > 0

/*
 * image/set payload:
 *   {"frames": [{"pixels": "RRGGBB...", "ms": 200}, ...],
 *    "loops": 0, "duration": 0, "brightness": 80}
 * pixels: six hex digits per LED, letters row by row from the top left,
 * optionally followed by the minute LEDs 1..4. loops 0 = endless, duration in
 * seconds 0 = no limit, brightness in percent (default: the clock's current
 * brightness).
 */
void Mqtt::processImage(const byte *payload, unsigned int length) {
    if (length > IMAGE_MAX_BYTES) {
        publishImageResult("message too large");
        return;
    }

    const uint16_t cells =
        usedClockType->rowsWordMatrix() * usedClockType->colsWordMatrix();
    const uint8_t extras = usedClockType->hasMinuteLeds() ? 4 : 0;
    if (imageOverlay == nullptr || imageOverlay->cells() != cells ||
        imageOverlay->pixels() != cells + extras) {
        delete imageOverlay;
        imageOverlay = new ImageOverlay(cells, extras, IMAGE_MAX_FRAMES);
    }

    DynamicJsonDocument doc(length + 2048);
    if (deserializeJson(doc, reinterpret_cast<const char *>(payload), length)) {
        publishImageResult("invalid JSON");
        return;
    }

    JsonArrayConst frames = doc["frames"].as<JsonArrayConst>();
    uint8_t brightness;
    if (doc.containsKey("brightness")) {
        brightness = static_cast<uint8_t>(
            constrain(doc["brightness"].as<int>(), 0, 100) * 255 / 100);
    } else {
        brightness = static_cast<uint8_t>(
            led.getColorbyPositionWithAppliedBrightness(Foreground).B * 255.f);
    }

    image::Result result = imageOverlay->begin(
        frames.isNull() ? 0 : min<size_t>(frames.size(), 255), doc["loops"] | 0,
        doc["duration"] | 0, brightness);
    if (result == image::Result::Ok && frames.size() > IMAGE_MAX_FRAMES) {
        result = image::Result::TooManyFrames;
    }
    uint8_t index = 0;
    for (JsonObjectConst frame : frames) {
        if (result != image::Result::Ok) {
            break;
        }
        result = imageOverlay->setFrame(index++, frame["pixels"] | "",
                                        frame["ms"] | 0);
    }
    if (result == image::Result::Ok) {
        result = imageOverlay->commit(millis());
    }

    if (result != image::Result::Ok) {
        publishImageResult(image::resultText(result));
        return;
    }
    publishImageResult(nullptr);
    // The newest message wins: a picture replaces a scrolling text.
    const bool replacedText = textOverlay.active();
    textOverlay.clear();
    if (mqttInstance) {
        mqttInstance->publishImageState();
        if (replacedText) {
            mqttInstance->publishTextState();
        }
    }
}

//------------------------------------------------------------------------------

void Mqtt::publishImageResult(const char *error) {
    StaticJsonDocument<160> doc;
    doc["accepted"] = error == nullptr;
    if (error) {
        doc["error"] = error;
    }
    char buffer[160];
    serializeJson(doc, buffer);
    mqttClient.publish(topic("image/result").c_str(), buffer, false);
}

//------------------------------------------------------------------------------

/* Retained, so an editor knows the limits before it sends anything. */
void Mqtt::publishImageInfo() {
    StaticJsonDocument<200> doc;
    doc["rows"] = usedClockType->rowsWordMatrix();
    doc["cols"] = usedClockType->colsWordMatrix();
    doc["minute_leds"] = usedClockType->hasMinuteLeds() ? 4 : 0;
    doc["max_bytes"] = IMAGE_MAX_BYTES;
    doc["max_frames"] = IMAGE_MAX_FRAMES;
    char buffer[200];
    serializeJson(doc, buffer);
    mqttClient.publish(topic("image/info").c_str(), buffer, true);
}

//------------------------------------------------------------------------------

void Mqtt::publishImageState() {
    const bool showing = imageOverlay && imageOverlay->active();
    mqttClient.publish(topic("image/state").c_str(), showing ? "ON" : "OFF",
                       true);
}

#else

void Mqtt::publishImageState() {}

#endif

//------------------------------------------------------------------------------
// Timer (timer/set, timer/clear, timer/state)
//------------------------------------------------------------------------------

/*
 * timer/set payload:
 *   {"seconds": 300, "variant": "A"|"B", "alarm": 30}   start (or restart)
 *   {"add": 60}                       add or (negative) take away seconds;
 *                                     starts the timer when none is running
 * Up to 99:59. alarm: seconds of red blinking at zero, default 30.
 * Needs a 10 x 11 front.
 */
void Mqtt::processTimer(const char *msg) {
    StaticJsonDocument<256> doc;
    const char *error = nullptr;
    if (deserializeJson(doc, msg)) {
        error = "invalid JSON";
    } else if (usedClockType->rowsWordMatrix() != timer::ROWS ||
               usedClockType->colsWordMatrix() != timer::COLS) {
        error = "the timer needs a 10 x 11 front";
    } else if (doc.containsKey("add")) {
        if (timerOverlay.add(doc["add"].as<long>(), millis()) !=
            timer::Error::None) {
            error = "nothing to add to";
        }
    } else {
        const char *variant = doc["variant"] | "B";
        const timer::Variant v = (variant[0] == 'A' || variant[0] == 'a')
                                     ? timer::Variant::A
                                     : timer::Variant::B;
        if (timerOverlay.start(doc["seconds"] | 0UL, v, doc["alarm"] | 30,
                               millis()) != timer::Error::None) {
            error = "seconds must be 1..5999";
        }
    }

    StaticJsonDocument<128> result;
    result["accepted"] = error == nullptr;
    if (error) {
        result["error"] = error;
    }
    char buffer[128];
    serializeJson(result, buffer);
    mqttClient.publish(topic("timer/result").c_str(), buffer, false);
    if (!error && mqttInstance) {
        mqttInstance->publishTimerState();
    }
}

//------------------------------------------------------------------------------

/* Retained; while running updated every second. */
void Mqtt::publishTimerState() {
    static const char *const names[] = {"idle", "running", "alarm"};
    StaticJsonDocument<160> doc;
    doc["state"] = names[static_cast<uint8_t>(timerOverlay.state())];
    doc["remaining"] = timerOverlay.remaining(millis());
    doc["total"] = timerOverlay.total();
    doc["variant"] = timerOverlay.variant() == timer::Variant::A ? "A" : "B";
    char buffer[160];
    serializeJson(doc, buffer);
    mqttClient.publish(topic("timer/state").c_str(), buffer, true);
}

//------------------------------------------------------------------------------
// Messages (text/set, text/clear, text/state, text/result)
//------------------------------------------------------------------------------

/*
 * text/set payload:
 *   {"text": "Alles Gute!", "color": "FF0000", "ms": 100, "loops": 1,
 *    "duration": 0, "brightness": 80}
 * Only text is required. UTF-8, up to TEXT_MAX_CHARS characters. color
 * RRGGBB (default: the clock's foreground colour), ms per column (default:
 * the effect speed), loops passes (default 1, 0 = endless), duration in
 * seconds (0 = no limit), brightness in percent (default: the clock's).
 * A newer picture or message replaces it; a running timer shows again after.
 */
void Mqtt::processText(const byte *payload, unsigned int length) {
    const char *error = nullptr;
    DynamicJsonDocument doc(length + 512);
    if (length > TEXT_MAX_BYTES) {
        error = "message too large";
    } else if (deserializeJson(doc, reinterpret_cast<const char *>(payload),
                               length)) {
        error = "invalid JSON";
    }

    if (error == nullptr) {
        HsbColor colour = G.color[Foreground];
        colour.B = 1.f;
        const char *hex = doc["color"] | "";
        if (*hex == '#') {
            hex++;
        }
        RgbColor rgb = colour;
        if (*hex != '\0' && !image::parseHexPixels(hex, 1, &rgb)) {
            error = "color must be RRGGBB";
        }

        float brightness;
        if (doc.containsKey("brightness")) {
            brightness = constrain(doc["brightness"].as<int>(), 0, 100) / 100.f;
        } else {
            brightness =
                led.getColorbyPositionWithAppliedBrightness(Foreground).B;
        }
        rgb = RgbColor(static_cast<uint8_t>(rgb.R * brightness + .5f),
                       static_cast<uint8_t>(rgb.G * brightness + .5f),
                       static_cast<uint8_t>(rgb.B * brightness + .5f));

        if (error == nullptr) {
            const text::Error e = textOverlay.start(
                doc["text"] | "", usedClockType->rowsWordMatrix(),
                usedClockType->colsWordMatrix(), rgb,
                doc["ms"] | effectSpeedIntervalMillis(G.effectSpeed),
                doc["loops"] | 1, doc["duration"] | 0, millis());
            if (e != text::Error::None) {
                error = text::errorText(e);
            }
        }
    }

    StaticJsonDocument<128> result;
    result["accepted"] = error == nullptr;
    if (error) {
        result["error"] = error;
    }
    char buffer[128];
    serializeJson(result, buffer);
    mqttClient.publish(topic("text/result").c_str(), buffer, false);
    if (error) {
        return;
    }

    // The newest message wins: a scrolling text replaces a picture.
#if IMAGE_MAX_FRAMES > 0
    const bool replacedImage = imageOverlay && imageOverlay->active();
    if (imageOverlay) {
        imageOverlay->clear();
    }
#else
    const bool replacedImage = false;
#endif
    if (mqttInstance) {
        mqttInstance->publishTextState();
        if (replacedImage) {
            mqttInstance->publishImageState();
        }
    }
}

//------------------------------------------------------------------------------

/* Retained: ON while a message is shown. */
void Mqtt::publishTextState() {
    mqttClient.publish(topic("text/state").c_str(),
                       textOverlay.active() ? "ON" : "OFF", true);
}
