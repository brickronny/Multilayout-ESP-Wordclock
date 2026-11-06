#pragma once

#include "Font.h"
#include "TextFont.h"

#include <NeoPixelBus.h>
#include <stdint.h>
#include <string.h>

/*
 * A message sent from outside (MQTT topic text/set): scrolls through the
 * 7 x 5 font from right to left, `loops` times, then hands the display back.
 * Like the picture overlay it sits on top of the active mode without touching
 * G.prog or the stored settings, so the clock simply continues afterwards.
 *
 * A text whose glyphs fit the front (two letters on 11 columns) stands still,
 * centred, for as long as one pass would take. The text is UTF-8 and converted
 * with TextFont, so umlauts show.
 */

#ifndef TEXT_MAX_CHARS
#define TEXT_MAX_CHARS 200
#endif

namespace text {

constexpr uint8_t GLYPH_WIDTH = 5;
constexpr uint8_t ADVANCE = GLYPH_WIDTH + 1; // one column between glyphs
constexpr uint8_t GLYPH_ROWS = 8;            // 7 rows plus a descender row
constexpr uint8_t FONT_HEIGHT = 7;
constexpr uint16_t MIN_MS = 20;
constexpr uint16_t MAX_MS = 1000;

enum class Error : uint8_t { None, Empty, TooLong, FrontTooSmall };

inline const char *errorText(Error e) {
    switch (e) {
    case Error::None:
        return "ok";
    case Error::Empty:
        return "no text";
    case Error::TooLong:
        return "text too long";
    case Error::FrontTooSmall:
        return "the front is too small for the font";
    }
    return "?";
}

} // namespace text

class TextOverlay {
public:
    /*
     * rows/cols: the front. ms: per column step (MIN_MS..MAX_MS).
     * loops: passes, 0 = endless; durationSeconds: 0 = no limit.
     * colour is shown as given (scale it for brightness before).
     */
    text::Error start(const char *utf8, uint8_t rows, uint8_t cols,
                      RgbColor colour, uint16_t ms, uint16_t loops,
                      uint16_t durationSeconds, uint32_t nowMs) {
        if (rows < text::FONT_HEIGHT || cols == 0) {
            return text::Error::FrontTooSmall;
        }
        char glyphs[TEXT_MAX_CHARS + 1];
        const size_t needed = textfont::toFont(utf8, glyphs, sizeof(glyphs));
        if (needed > TEXT_MAX_CHARS) {
            return text::Error::TooLong;
        }
        // Spaces at either end would only delay or stretch a pass.
        size_t first = 0;
        size_t end = needed;
        while (first < end && glyphs[first] == ' ') {
            first++;
        }
        while (end > first && glyphs[end - 1] == ' ') {
            end--;
        }
        if (first == end) {
            return text::Error::Empty;
        }

        m_length = static_cast<uint16_t>(end - first);
        memcpy(m_glyphs, glyphs + first, m_length);
        m_rows = rows;
        m_cols = cols;
        m_colour = colour;
        m_ms = ms < text::MIN_MS ? text::MIN_MS
                                 : (ms > text::MAX_MS ? text::MAX_MS : ms);
        m_loops = loops;
        m_durationMs = static_cast<uint32_t>(durationSeconds) * 1000;
        m_width = m_length * text::ADVANCE - 1;
        m_static = m_width <= m_cols;
        m_steps = m_cols + m_width;
        m_startMs = nowMs;
        m_shownStep = -1;
        m_active = true;
        m_stopRequested = false;
        return text::Error::None;
    }

    bool active() const { return m_active; }

    /* End now, without a last update (a picture replaces the text). */
    void clear() {
        m_active = false;
        m_stopRequested = false;
    }

    /* End at the next update() (text/clear), so the display is handed back. */
    void stop() { m_stopRequested = m_active; }

    uint16_t length() const { return m_length; }
    bool isStatic() const { return m_static; }
    uint16_t width() const { return m_width; }
    /* Milliseconds of one pass. */
    uint32_t passMs() const { return static_cast<uint32_t>(m_steps) * m_ms; }

    /*
     * Draw the frame for nowMs into `frame` (rows x cols, row by row from the
     * top left). Returns false once the message is over. `changed` tells
     * whether the frame differs from the last call; frame is written only
     * then.
     */
    bool update(uint32_t nowMs, RgbColor *frame, bool &changed) {
        changed = false;
        if (!m_active) {
            return false;
        }
        if (m_stopRequested) {
            clear();
            return false;
        }
        const uint32_t elapsed = nowMs - m_startMs; // wraps correctly
        const bool timedOut = m_durationMs > 0 && elapsed >= m_durationMs;
        const bool loopsDone =
            m_loops > 0 && elapsed >= static_cast<uint64_t>(passMs()) * m_loops;
        if (timedOut || loopsDone) {
            clear();
            return false;
        }

        const int32_t step =
            m_static ? 0 : static_cast<int32_t>((elapsed / m_ms) % m_steps);
        if (step == m_shownStep) {
            return true;
        }
        m_shownStep = step;
        changed = true;

        // Scrolling: at step 0 the text starts just right of the front, one
        // column enters per step, the pass ends when the last column has
        // left on the left.
        const int32_t x0 = m_static ? (m_cols - m_width) / 2 : m_cols - step;
        render(frame, x0);
        return true;
    }

    /* Draw the text with its first column at x0 (may be off the front). */
    void render(RgbColor *frame, int32_t x0) const {
        const uint8_t top = (m_rows - text::FONT_HEIGHT) / 2;
        for (uint16_t i = 0; i < static_cast<uint16_t>(m_rows) * m_cols; i++) {
            frame[i] = RgbColor(0, 0, 0);
        }
        for (uint8_t x = 0; x < m_cols; x++) {
            const int32_t column = x - x0;
            if (column < 0 || column >= m_width) {
                continue;
            }
            const uint8_t inGlyph = column % text::ADVANCE;
            if (inGlyph >= text::GLYPH_WIDTH) {
                continue; // the gap between two glyphs
            }
            const uint8_t glyph =
                static_cast<uint8_t>(m_glyphs[column / text::ADVANCE]);
            const uint8_t bits = pgm_read_byte(&(font_7x5[glyph][inGlyph]));
            for (uint8_t bit = 0; bit < text::GLYPH_ROWS; bit++) {
                const uint8_t row = top + bit;
                if (row < m_rows && (bits >> bit & 1)) {
                    frame[row * m_cols + x] = m_colour;
                }
            }
        }
    }

private:
    char m_glyphs[TEXT_MAX_CHARS];
    uint16_t m_length = 0;
    uint16_t m_width = 0;
    uint16_t m_steps = 0;
    uint8_t m_rows = 0;
    uint8_t m_cols = 0;
    RgbColor m_colour = RgbColor(0, 0, 0);
    uint16_t m_ms = 100;
    uint16_t m_loops = 1;
    uint32_t m_durationMs = 0;
    uint32_t m_startMs = 0;
    int32_t m_shownStep = -1;
    bool m_static = false;
    bool m_active = false;
    bool m_stopRequested = false;
};
