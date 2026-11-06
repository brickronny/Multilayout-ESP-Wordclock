#pragma once

#include <NeoPixelBus.h>
#include <stdint.h>

/*
 * A countdown started from outside (MQTT topic timer/set), shown on top of
 * the current mode on a 10 x 11 front, like ImageOverlay. Variant A stacks
 * minutes (top) and seconds (bottom) in 3x5 digits; variant B shows big 4x7
 * minutes inside a 38-pixel seconds ring and switches to big seconds in the
 * last minute. The last 10 s are red; at zero the display blinks red for the
 * alarm time, then the clock takes over again.
 */

namespace timer {

const uint8_t ROWS = 10;
const uint8_t COLS = 11;
const uint16_t CELLS = ROWS * COLS;
const uint16_t PIXELS = CELLS + 4; // + the minute LEDs, kept dark

const uint32_t MAX_SECONDS = 99 * 60 + 59;

// Pure W on a warm white RGBW strip (its own colour, 255/180/107).
const RgbColor MINUTES(0xFF, 0xB4, 0x6B);
const RgbColor SECONDS(0xFF, 0x8A, 0x1F);
const RgbColor ALERT(0xFF, 0x3B, 0x3B);
const RgbColor DIM(0x3D, 0x3C, 0x38);
const RgbColor OFF(0, 0, 0);

enum class Variant : uint8_t { A, B };
enum class State : uint8_t { Idle, Running, Alarm };
enum class Error : uint8_t { None, BadDuration };

// 3x5 digits, one row per entry, bit 2 = left column.
const uint8_t DIGITS_3X5[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7},
    {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 2, 2, 2},
    {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
};

// 4x7 digits, one row per entry, bit 3 = left column.
const uint8_t DIGITS_4X7[10][7] = {
    {6, 9, 9, 9, 9, 9, 6},   {2, 6, 2, 2, 2, 2, 7},  {6, 9, 1, 2, 4, 8, 15},
    {14, 1, 1, 6, 1, 1, 14}, {9, 9, 9, 15, 1, 1, 1}, {15, 8, 14, 1, 1, 9, 6},
    {6, 8, 8, 14, 9, 9, 6},  {15, 1, 2, 2, 4, 4, 4}, {6, 9, 9, 6, 9, 9, 6},
    {6, 9, 9, 7, 1, 1, 6},
};

inline void set(RgbColor *frame, uint8_t row, uint8_t col, RgbColor c) {
    frame[row * COLS + col] = c;
}

inline void digit3x5(RgbColor *frame, uint8_t value, uint8_t row0, uint8_t col0,
                     RgbColor c) {
    for (uint8_t r = 0; r < 5; r++) {
        for (uint8_t k = 0; k < 3; k++) {
            if (DIGITS_3X5[value][r] & (4 >> k)) {
                set(frame, row0 + r, col0 + k, c);
            }
        }
    }
}

inline void digit4x7(RgbColor *frame, uint8_t value, uint8_t row0, uint8_t col0,
                     RgbColor c) {
    for (uint8_t r = 0; r < 7; r++) {
        for (uint8_t k = 0; k < 4; k++) {
            if (DIGITS_4X7[value][r] & (8 >> k)) {
                set(frame, row0 + r, col0 + k, c);
            }
        }
    }
}

/* Ring position i (0..37), clockwise from the top middle. */
inline void ringCell(uint8_t i, uint8_t &row, uint8_t &col) {
    if (i < 6) { // top row, cols 5..10
        row = 0;
        col = 5 + i;
    } else if (i < 15) { // right column, rows 1..9
        row = i - 5;
        col = 10;
    } else if (i < 25) { // bottom row, cols 9..0
        row = 9;
        col = 24 - i;
    } else if (i < 34) { // left column, rows 8..0
        row = 33 - i;
        col = 0;
    } else { // top row, cols 1..4
        row = 0;
        col = i - 33;
    }
}

/*
 * remaining in seconds; alarmOn only matters at 0 (blink phase). Fills
 * PIXELS colours: the letters row by row, then the four minute LEDs.
 */
inline void render(uint32_t remaining, Variant variant, bool alarmOn,
                   RgbColor *frame) {
    for (uint16_t i = 0; i < PIXELS; i++) {
        frame[i] = OFF;
    }
    if (remaining == 0 && !alarmOn) {
        return;
    }
    const bool red = remaining < 10;
    const uint32_t minutes = remaining / 60;
    const uint8_t seconds = remaining % 60;

    if (variant == Variant::A) {
        const RgbColor top = red ? ALERT : MINUTES;
        const RgbColor bottom = red ? ALERT : SECONDS;
        digit3x5(frame, minutes / 10, 0, 2, top);
        digit3x5(frame, minutes % 10, 0, 6, top);
        digit3x5(frame, seconds / 10, 5, 2, bottom);
        digit3x5(frame, seconds % 10, 5, 6, bottom);
        return;
    }

    const uint8_t lit = (seconds * 38 * 2 + 60) / 120; // round(s / 60 * 38)
    for (uint8_t i = 0; i < 38; i++) {
        uint8_t row, col;
        ringCell(i, row, col);
        set(frame, row, col, i < lit ? (red ? ALERT : SECONDS) : DIM);
    }
    const bool bigSeconds = minutes == 0;
    const uint32_t shown = bigSeconds ? seconds : minutes;
    const RgbColor c = red ? ALERT : (bigSeconds ? SECONDS : MINUTES);
    digit4x7(frame, shown / 10, 1, 1, c);
    digit4x7(frame, shown % 10, 1, 6, c);
}

} // namespace timer

class TimerOverlay {
public:
    timer::Error start(uint32_t seconds, timer::Variant variant,
                       uint16_t alarmSeconds, uint32_t nowMs) {
        if (seconds == 0 || seconds > timer::MAX_SECONDS) {
            return timer::Error::BadDuration;
        }
        m_variant = variant;
        m_alarmMs = static_cast<uint32_t>(alarmSeconds) * 1000;
        m_total = seconds;
        m_endMs = nowMs + seconds * 1000;
        m_state = timer::State::Running;
        m_shown = UINT32_MAX;
        return timer::Error::None;
    }

    /* Adds (or with a negative value takes away) time; starts if idle. */
    timer::Error add(int32_t seconds, uint32_t nowMs) {
        if (m_state != timer::State::Running) {
            return seconds > 0
                       ? start(seconds, m_variant, m_alarmMs / 1000, nowMs)
                       : timer::Error::BadDuration;
        }
        int64_t left = static_cast<int64_t>(remainingMs(nowMs)) +
                       static_cast<int64_t>(seconds) * 1000;
        if (left < 0) {
            left = 0;
        }
        if (left > static_cast<int64_t>(timer::MAX_SECONDS) * 1000) {
            left = static_cast<int64_t>(timer::MAX_SECONDS) * 1000;
        }
        m_endMs = nowMs + static_cast<uint32_t>(left);
        m_total = (static_cast<uint32_t>(left) + 999) / 1000;
        m_shown = UINT32_MAX;
        return timer::Error::None;
    }

    void stop() { m_state = timer::State::Idle; }

    /* Redraw at the next update (something else covered the display). */
    void invalidate() { m_shown = UINT32_MAX; }

    timer::State state() const { return m_state; }
    timer::Variant variant() const { return m_variant; }
    uint32_t total() const { return m_total; }

    /* Seconds left, rounded up: 5:00 for the whole first second. */
    uint32_t remaining(uint32_t nowMs) const {
        return (remainingMs(nowMs) + 999) / 1000;
    }

    /*
     * Advances the state and fills `frame` (timer::PIXELS colours) while the
     * timer is running or ringing. `changed` is set when the picture differs
     * from the previous call.
     */
    timer::State update(uint32_t nowMs, RgbColor *frame, bool &changed) {
        changed = false;
        if (m_state == timer::State::Running && remainingMs(nowMs) == 0) {
            m_state = timer::State::Alarm;
            m_alarmStartMs = nowMs;
        }
        if (m_state == timer::State::Alarm &&
            nowMs - m_alarmStartMs >= m_alarmMs) {
            m_state = timer::State::Idle;
        }
        if (m_state == timer::State::Idle) {
            return m_state;
        }

        uint32_t key; // what is on display: seconds left, or blink phase
        bool alarmOn = false;
        if (m_state == timer::State::Running) {
            key = remaining(nowMs);
        } else {
            alarmOn = ((nowMs - m_alarmStartMs) / 500) % 2 == 0;
            key = alarmOn ? UINT32_MAX - 1 : UINT32_MAX - 2;
        }
        if (key != m_shown) {
            m_shown = key;
            changed = true;
            timer::render(m_state == timer::State::Running ? key : 0, m_variant,
                          alarmOn, frame);
        }
        return m_state;
    }

private:
    uint32_t remainingMs(uint32_t nowMs) const {
        if (m_state != timer::State::Running) {
            return 0;
        }
        const int32_t left = static_cast<int32_t>(m_endMs - nowMs);
        return left > 0 ? static_cast<uint32_t>(left) : 0;
    }

    timer::State m_state = timer::State::Idle;
    timer::Variant m_variant = timer::Variant::B;
    uint32_t m_endMs = 0;
    uint32_t m_total = 0;
    uint32_t m_alarmMs = 30000;
    uint32_t m_alarmStartMs = 0;
    uint32_t m_shown = UINT32_MAX;
};
