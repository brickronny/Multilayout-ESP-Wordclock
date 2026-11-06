/*
 * TextFont: UTF-8 from Home Assistant, MQTT or the web page into indexes of
 * the 7 x 5 font, whose upper half holds code page 437.
 */

#include "Font.h"
#include "TextFont.h"

#include "support/check.h"

#include <string>

namespace {

std::string font(const char *utf8, size_t size = 64) {
    char out[64];
    textfont::toFont(utf8, out, size);
    return out;
}

std::string bytes(std::initializer_list<int> list) {
    std::string s;
    for (int b : list) {
        s += static_cast<char>(b);
    }
    return s;
}

/* A glyph as 8 rows of '#' and '.', top row first. */
std::string glyph(unsigned char index) {
    std::string s;
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 5; col++) {
            s += (pgm_read_byte(&font_7x5[index][col]) >> row & 1) ? '#' : '.';
        }
        s += '\n';
    }
    return s;
}

void asciiPassesThrough() {
    check(font("HELLO world 12:34!") == "HELLO world 12:34!",
          "ASCII stays as it is");
    check(font("") == "", "an empty text stays empty");
    check(font(nullptr) == "", "no text gives an empty text");
}

void germanLetters() {
    check(font("\xC3\xA4\xC3\xB6\xC3\xBC") == bytes({132, 148, 129}),
          "ä ö ü map to CP437 132 148 129");
    check(font("\xC3\x84\xC3\x96\xC3\x9C") == bytes({142, 153, 154}),
          "Ä Ö Ü map to CP437 142 153 154");
    check(font("Stra\xC3\x9F"
               "e") == "Stra" + bytes({225}) + "e",
          "ß maps to CP437 225");
    check(font("21\xC2\xB0") == "21" + bytes({248}), "° maps to CP437 248");
    check(font("Caf\xC3\xA9") == "Caf" + bytes({130}), "é maps to CP437 130");
}

void glyphsAreTheRightOnes() {
    // The font comments name Windows-1252 characters; the glyphs must really
    // be the CP437 ones the table relies on.
    check(glyph(132) == ".#.#.\n"
                        ".....\n"
                        ".###.\n"
                        "....#\n"
                        ".####\n"
                        "#...#\n"
                        ".####\n"
                        ".....\n",
          "glyph 132 is ä");
    check(glyph(148) == ".#.#.\n"
                        ".....\n"
                        ".###.\n"
                        "#...#\n"
                        "#...#\n"
                        "#...#\n"
                        ".###.\n"
                        ".....\n",
          "glyph 148 is ö");
    check(glyph(142) == "#...#\n"
                        "..#..\n"
                        ".#.#.\n"
                        "#...#\n"
                        "#####\n"
                        "#...#\n"
                        "#...#\n"
                        ".....\n",
          "glyph 142 is Ä");
    check(glyph(225) == ".###.\n"
                        "#...#\n"
                        "#...#\n"
                        "#.##.\n"
                        "#...#\n"
                        "#...#\n"
                        "#..#.\n"
                        ".....\n",
          "glyph 225 is ß");
    check(glyph(248) == ".###.\n"
                        ".#.#.\n"
                        ".###.\n"
                        ".....\n"
                        ".....\n"
                        ".....\n"
                        ".....\n"
                        ".....\n",
          "glyph 248 is °");
}

void typography() {
    check(font("\xE2\x80\x9E"
               "Hallo\xE2\x80\x9C") == "\"Hallo\"",
          "German quotes become plain quotes");
    check(font("a \xE2\x80\x93 b") == "a - b", "en dash becomes a hyphen");
    check(font("it\xE2\x80\x99s") == "it's", "apostrophe becomes '");
    check(font("5\xE2\x82\xAC") == "5E", "euro sign becomes E");
    check(font("a\nb\tc") == "a b c", "control characters become spaces");
}

void unknownAndBroken() {
    check(font("\xF0\x9F\x98\x80!") == "?!", "an emoji becomes one ?");
    check(font("\xE4"
               "b") == "?b",
          "a lone Latin-1 byte (not UTF-8) becomes ?");
    check(font("a\xC3") == "a?", "a cut sequence at the end becomes ?");
    check(font("\xC3(") == "?(", "a broken sequence skips only one byte");
}

void cutToBuffer() {
    char out[4];
    const size_t needed = textfont::toFont("\xC3\xA4"
                                           "bcdef",
                                           out, sizeof(out));
    check(needed == 6, "toFont counts glyphs, not bytes");
    check(std::string(out) == bytes({132}) + "bc",
          "a long text is cut to the buffer and terminated");

    // The scrolling mode stores 29 bytes of UTF-8; converted it never needs
    // more glyphs than bytes.
    char same[30];
    check(textfont::toFont("\xC3\xA4\xC3\xB6\xC3\xBC", same, sizeof(same)) == 3,
          "UTF-8 never takes more glyphs than bytes");
}

} // namespace

int main() {
    asciiPassesThrough();
    germanLetters();
    glyphsAreTheRightOnes();
    typography();
    unknownAndBroken();
    cutToBuffer();

    return report("text font");
}
