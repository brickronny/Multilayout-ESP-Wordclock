#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Text for the 7 x 5 font (Font.h). Above ASCII that font holds the glyphs of
 * code page 437 (the comments there name Windows-1252 characters, the glyphs
 * are CP437), while Home Assistant, MQTT and the web page send UTF-8. Without
 * this conversion an "ä" (C3 A4) shows as two unrelated glyphs.
 *
 * toFont() turns UTF-8 into font indexes, one byte per glyph. Characters the
 * font lacks become '?', typographic quotes and dashes their plain ASCII
 * counterparts, control characters a space. Converting at draw time keeps the
 * stored text UTF-8, so what the clock reports back stays valid.
 */

namespace textfont {

constexpr char UNKNOWN = '?';

/* Font index for a Unicode code point, 0 when there is none. */
inline uint8_t glyphOf(uint32_t cp) {
    if (cp >= 0x20 && cp < 0x7F) {
        return static_cast<uint8_t>(cp);
    }
    if (cp < 0x20 || cp == 0x7F || cp == 0xA0) {
        return ' ';
    }
    struct Pair {
        uint16_t cp;
        uint8_t glyph;
    };
    // clang-format off
    static const Pair table[] = {
        // Latin-1 letters and signs present in the font (CP437 positions)
        {0xC7, 128}, {0xFC, 129}, {0xE9, 130}, {0xE2, 131}, {0xE4, 132},
        {0xE0, 133}, {0xE5, 134}, {0xE7, 135}, {0xEA, 136}, {0xEB, 137},
        {0xE8, 138}, {0xEF, 139}, {0xEE, 140}, {0xEC, 141}, {0xC4, 142},
        {0xC5, 143}, {0xC9, 144}, {0xF4, 147}, {0xF6, 148}, {0xF2, 149},
        {0xFB, 150}, {0xF9, 151}, {0xFF, 152}, {0xD6, 153}, {0xDC, 154},
        {0xA2, 155}, {0xA3, 156}, {0xE1, 160}, {0xED, 161}, {0xF3, 162},
        {0xFA, 163}, {0xF1, 164}, {0xD1, 165}, {0xAA, 166}, {0xBA, 167},
        {0xBF, 168}, {0xBD, 171}, {0xBC, 172}, {0xA1, 173}, {0xAB, 174},
        {0xBB, 175}, {0xDF, 225}, {0xB5, 230}, {0xB1, 241}, {0xF7, 246},
        {0xB0, 248}, {0xB2, 253},
        // Typography without a glyph of its own
        {0x2018, '\''}, {0x2019, '\''}, {0x201A, '\''}, {0x201C, '"'},
        {0x201D, '"'}, {0x201E, '"'}, {0x2013, '-'}, {0x2014, '-'},
        {0x2026, '.'}, {0x20AC, 'E'}, {0x1E9E, 225}, // capital sharp s
    };
    // clang-format on
    for (const Pair &p : table) {
        if (p.cp == cp) {
            return p.glyph;
        }
    }
    return 0;
}

/*
 * Decode one UTF-8 character at s. Returns the bytes it takes (at least 1)
 * and the code point, or 0xFFFFFFFF for a broken sequence (one byte is then
 * skipped).
 */
inline uint8_t decode(const char *s, uint32_t &cp) {
    const uint8_t b0 = static_cast<uint8_t>(s[0]);
    uint8_t need;
    if (b0 < 0x80) {
        cp = b0;
        return 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        need = 1;
        cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        need = 2;
        cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        need = 3;
        cp = b0 & 0x07;
    } else {
        cp = 0xFFFFFFFF;
        return 1;
    }
    for (uint8_t i = 1; i <= need; i++) {
        const uint8_t b = static_cast<uint8_t>(s[i]);
        if ((b & 0xC0) != 0x80) { // also stops at the terminating zero
            cp = 0xFFFFFFFF;
            return 1;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    return need + 1;
}

/*
 * Convert UTF-8 into font indexes, zero-terminated, at most outSize - 1
 * glyphs. Returns the number of glyphs the whole text needs, so a result
 * >= outSize means it was cut.
 */
inline size_t toFont(const char *utf8, char *out, size_t outSize) {
    size_t n = 0;
    if (utf8 != nullptr) {
        for (const char *s = utf8; *s != '\0';) {
            uint32_t cp;
            s += decode(s, cp);
            uint8_t glyph = cp == 0xFFFFFFFF ? 0 : glyphOf(cp);
            if (glyph == 0) {
                glyph = UNKNOWN;
            }
            if (n + 1 < outSize) {
                out[n] = static_cast<char>(glyph);
            }
            n++;
        }
    }
    if (outSize > 0) {
        out[n < outSize ? n : outSize - 1] = '\0';
    }
    return n;
}

} // namespace textfont
