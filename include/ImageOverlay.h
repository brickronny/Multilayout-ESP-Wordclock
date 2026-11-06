#pragma once

#include <NeoPixelBus.h>
#include <new>
#include <stdint.h>
#include <string.h>

/*
 * Pictures and animations sent from outside (MQTT topic image/set), shown on
 * top of whatever mode is active and ended by their loop count, their
 * duration or image/clear. The clock's own mode (G.prog) is not touched, so
 * nothing of it is persisted and the clock picks up where it left off.
 *
 * A frame is one hex string with six digits (RRGGBB) per pixel: the letters
 * row by row from the top left, optionally followed by the minute LEDs in
 * minute order (1..4). Loading happens into a pending buffer; a broken message
 * never disturbs the image that is showing.
 */

namespace image {

enum class Result : uint8_t {
    Ok,
    NoFrames,
    TooManyFrames,
    BadFrameIndex,
    BadPixels,
    MissingFrame,
    NoMemory,
};

inline const char *resultText(Result r) {
    switch (r) {
    case Result::Ok:
        return "ok";
    case Result::NoFrames:
        return "no frames";
    case Result::TooManyFrames:
        return "too many frames";
    case Result::BadFrameIndex:
        return "bad frame index";
    case Result::BadPixels:
        return "pixels must be 6 hex digits per LED (letters, or letters + "
               "minute LEDs)";
    case Result::MissingFrame:
        return "a frame is missing";
    case Result::NoMemory:
        return "out of memory";
    }
    return "?";
}

inline int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Exactly `count` pixels of RRGGBB, nothing more, nothing less. */
inline bool parseHexPixels(const char *text, uint16_t count, RgbColor *out) {
    if (text == nullptr || strlen(text) != static_cast<size_t>(count) * 6) {
        return false;
    }
    for (uint16_t i = 0; i < count; i++) {
        uint8_t channel[3];
        for (uint8_t c = 0; c < 3; c++) {
            const int hi = hexDigit(text[i * 6 + c * 2]);
            const int lo = hexDigit(text[i * 6 + c * 2 + 1]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            channel[c] = static_cast<uint8_t>(hi * 16 + lo);
        }
        out[i].R = channel[0];
        out[i].G = channel[1];
        out[i].B = channel[2];
    }
    return true;
}

} // namespace image

class ImageOverlay {
public:
    /* cells = letters, extras = minute LEDs that may follow them */
    ImageOverlay(uint16_t cells, uint8_t extras, uint8_t maxFrames)
        : m_cells(cells), m_pixels(cells + extras), m_maxFrames(maxFrames) {}

    ~ImageOverlay() {
        release(m_active);
        release(m_pending);
    }

    ImageOverlay(const ImageOverlay &) = delete;
    ImageOverlay &operator=(const ImageOverlay &) = delete;

    uint16_t pixels() const { return m_pixels; }
    uint16_t cells() const { return m_cells; }
    uint8_t maxFrames() const { return m_maxFrames; }

    /*
     * loops: 0 = endless; durationSeconds: 0 = no limit. Without both the
     * overlay stays until clear() or a new image. brightness 0..255 scales
     * all colours.
     */
    image::Result begin(uint8_t frameCount, uint16_t loops,
                        uint16_t durationSeconds, uint8_t brightness) {
        release(m_pending);
        if (frameCount == 0) {
            return image::Result::NoFrames;
        }
        if (frameCount > m_maxFrames) {
            return image::Result::TooManyFrames;
        }
        m_pending.colours = new (std::nothrow)
            RgbColor[static_cast<uint32_t>(frameCount) * m_pixels];
        m_pending.ms = new (std::nothrow) uint16_t[frameCount];
        m_pending.loaded = new (std::nothrow) bool[frameCount];
        if (!m_pending.colours || !m_pending.ms || !m_pending.loaded) {
            release(m_pending);
            return image::Result::NoMemory;
        }
        for (uint8_t i = 0; i < frameCount; i++) {
            m_pending.loaded[i] = false;
            m_pending.ms[i] = 0;
        }
        m_pending.frames = frameCount;
        m_pending.loops = loops;
        m_pending.durationMs = static_cast<uint32_t>(durationSeconds) * 1000;
        m_pending.brightness = brightness;
        return image::Result::Ok;
    }

    image::Result setFrame(uint8_t index, const char *hex, uint16_t ms) {
        if (m_pending.colours == nullptr || index >= m_pending.frames) {
            return image::Result::BadFrameIndex;
        }
        RgbColor *dest = m_pending.colours + index * m_pixels;
        const size_t length = hex ? strlen(hex) : 0;
        uint16_t count;
        if (length == static_cast<size_t>(m_pixels) * 6) {
            count = m_pixels;
        } else if (length == static_cast<size_t>(m_cells) * 6) {
            count = m_cells;
        } else {
            return image::Result::BadPixels;
        }
        if (!image::parseHexPixels(hex, count, dest)) {
            return image::Result::BadPixels;
        }
        for (uint16_t i = count; i < m_pixels; i++) {
            dest[i] = RgbColor(0, 0, 0);
        }
        m_pending.ms[index] = ms;
        m_pending.loaded[index] = true;
        return image::Result::Ok;
    }

    /* Make the pending image the one that is shown, starting at nowMs. */
    image::Result commit(uint32_t nowMs) {
        if (m_pending.colours == nullptr) {
            return image::Result::NoFrames;
        }
        for (uint8_t i = 0; i < m_pending.frames; i++) {
            if (!m_pending.loaded[i]) {
                release(m_pending);
                return image::Result::MissingFrame;
            }
        }
        if (m_pending.brightness != 255) {
            const uint32_t n =
                static_cast<uint32_t>(m_pending.frames) * m_pixels;
            for (uint32_t i = 0; i < n; i++) {
                RgbColor &c = m_pending.colours[i];
                c.R = scale(c.R, m_pending.brightness);
                c.G = scale(c.G, m_pending.brightness);
                c.B = scale(c.B, m_pending.brightness);
            }
        }
        release(m_active);
        m_active = m_pending;
        m_pending = Slot();
        m_cycleMs = 0;
        for (uint8_t i = 0; i < m_active.frames; i++) {
            m_cycleMs += m_active.ms[i];
        }
        m_startMs = nowMs;
        m_shownFrame = -1;
        m_stopRequested = false;
        return image::Result::Ok;
    }

    void clear() {
        release(m_active);
        release(m_pending);
        m_stopRequested = false;
    }

    /* End the overlay at the next update() (image/clear). */
    void stop() { m_stopRequested = active(); }

    bool active() const { return m_active.colours != nullptr; }

    /*
     * The frame to show at nowMs, or nullptr once the overlay is over (it is
     * then released). `changed` tells whether it differs from the last call.
     */
    const RgbColor *update(uint32_t nowMs, bool &changed) {
        changed = false;
        if (!active()) {
            return nullptr;
        }
        if (m_stopRequested) {
            m_stopRequested = false;
            release(m_active);
            return nullptr;
        }
        const uint32_t elapsed = nowMs - m_startMs; // wraps correctly
        const bool timedOut =
            m_active.durationMs > 0 && elapsed >= m_active.durationMs;
        const bool loopsDone =
            m_active.loops > 0 && m_cycleMs > 0 &&
            elapsed >= static_cast<uint64_t>(m_cycleMs) * m_active.loops;
        if (timedOut || loopsDone) {
            release(m_active);
            return nullptr;
        }

        uint8_t index = 0;
        if (m_cycleMs > 0) {
            uint32_t position = elapsed % m_cycleMs;
            while (index + 1 < m_active.frames &&
                   position >= m_active.ms[index]) {
                position -= m_active.ms[index];
                index++;
            }
        }
        changed = index != m_shownFrame;
        m_shownFrame = index;
        return m_active.colours + index * m_pixels;
    }

private:
    struct Slot {
        RgbColor *colours = nullptr;
        uint16_t *ms = nullptr;
        bool *loaded = nullptr;
        uint8_t frames = 0;
        uint16_t loops = 0;
        uint32_t durationMs = 0;
        uint8_t brightness = 255;
    };

    static void release(Slot &slot) {
        delete[] slot.colours;
        delete[] slot.ms;
        delete[] slot.loaded;
        slot = Slot();
    }

    static uint8_t scale(uint8_t value, uint8_t brightness) {
        return static_cast<uint8_t>((value * brightness + 127) / 255);
    }

    uint16_t m_cells;
    uint16_t m_pixels;
    uint8_t m_maxFrames;
    Slot m_active;
    Slot m_pending;
    uint32_t m_cycleMs = 0;
    uint32_t m_startMs = 0;
    int16_t m_shownFrame = -1;
    bool m_stopRequested = false;
};
