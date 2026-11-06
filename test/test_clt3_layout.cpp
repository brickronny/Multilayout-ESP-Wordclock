/*
 * Ger10x11Clt3: the Leuchtbildshop CLT3 kit wires its strip column by column
 * and splices the four corner (minute) LEDs into the chain.
 *
 * The expected indices below are written out by hand from the kit's wiring
 * (column 0 top -> bottom, column 1 bottom -> top, ...; corners at 0, 11, 102
 * and 113), not derived from the formula under test. The kit manual's own
 * check is "after the LED test LED 2 lights up: the E of ES", i.e. index 1.
 */

#include "WordClockState.h"

#include "WordClockTypes/ClockType.hpp"

// Globals the layouts expect from the unity build.
GLOBAL G = {};
uint32_t frontMatrix[MAX_ROW_SIZE] = {0};
uint8_t frontWordId[MAX_ROW_SIZE][MAX_COL_SIZE] = {{WORD_ID_NONE}};

#include "AllLayouts.gen.h"

#include "support/check.h"

#include <set>
#include <string.h>

namespace {

char message[200];
ClockType &clt3 = _de10x11Clt3;

void setUp() {
    G.buildTypeDef = BuildTypeDef::Normal;
    G.minuteVariant = MinuteVariant::LED4x;
    G.minuteLedCount = MINUTE_LEDS_WIRED_4;
    G.secondsFrameLedCount = 0;
    memset(frontMatrix, 0, sizeof(frontMatrix));
    ClockType::resetWordIds();
}

void expectIndex(uint8_t row, uint8_t col, uint16_t expected,
                 const char *what) {
    const uint16_t got = clt3.getFrontMatrixIndex(row, col);
    snprintf(message, sizeof(message), "%s (row %u, col %u): index %u, got %u",
             what, row, col, expected, got);
    check(got == expected, message);
}

void anchorsFromTheKitWiring() {
    setUp();
    expectIndex(0, 0, 1, "E of ES, the manual's LED 2");
    expectIndex(1, 0, 2, "Z of ZEHN, below it");
    expectIndex(9, 0, 10, "Z of ZEHN (bottom row), end of column 0");
    expectIndex(9, 1, 12, "column 1 starts at the bottom, after corner LU");
    expectIndex(0, 1, 21, "S of ES, top of column 1");
    expectIndex(0, 2, 22, "K, top of column 2");
    expectIndex(0, 9, 101, "N of FÜNF, top of column 9");
    expectIndex(0, 10, 103, "F of FÜNF, after corner RO");
    expectIndex(9, 10, 112, "R of UHR, last letter before corner RU");
}

void minuteLedsAreTheCorners() {
    setUp();
    uint16_t minutes[4] = {0};
    clt3.getMinuteArray(minutes, MinuteVariant::LED4x);
    check(minutes[0] == 0, "minute 1 is corner top left (0)");
    check(minutes[1] == 102, "minute 2 is corner top right (102)");
    check(minutes[2] == 113, "minute 3 is corner bottom right (113)");
    check(minutes[3] == 11, "minute 4 is corner bottom left (11)");
    check(!clt3.supportsMinuteVariant(MinuteVariant::LED7x),
          "LED7x is not offered: the kit has exactly four corner LEDs");
    check(!clt3.supportsSecondsFrame(), "no seconds frame on this kit");
}

void everyLedUsedExactlyOnce() {
    setUp();
    std::set<uint16_t> seen;
    bool duplicate = false;
    for (uint8_t row = 0; row < clt3.rowsWordMatrix(); row++) {
        for (uint8_t col = 0; col < clt3.colsWordMatrix(); col++) {
            duplicate |=
                !seen.insert(clt3.getFrontMatrixIndex(row, col)).second;
        }
    }
    uint16_t minutes[4] = {0};
    clt3.getMinuteArray(minutes, MinuteVariant::LED4x);
    for (uint16_t m : minutes) {
        duplicate |= !seen.insert(m).second;
    }

    check(!duplicate, "no two letters or corners share an LED");
    check(seen.size() == 114, "110 letters + 4 corners = 114 LEDs");
    check(*seen.begin() == 0 && *seen.rbegin() == 113,
          "the LEDs are exactly 0..113");

    snprintf(message, sizeof(message), "strip is sized to 114 LEDs (got %u)",
             clt3.numPixelsOnStrip());
    check(clt3.numPixelsOnStrip() == 114, message);
}

/*
 * The render path: a word sets bits via setFrontMatrixWord(), the renderer
 * reads them back with getFrontMatrixPixel(row, col) and hands the same
 * (row, col) to getFrontMatrixIndex() (Led::setbyColorMatrix, no mirroring).
 */
std::set<uint16_t> ledsOf(FrontWord word) {
    setUp();
    clt3.show(word);
    std::set<uint16_t> leds;
    for (uint8_t row = 0; row < clt3.rowsWordMatrix(); row++) {
        for (uint8_t col = 0; col < clt3.colsWordMatrix(); col++) {
            if (clt3.getFrontMatrixPixel(row, col)) {
                leds.insert(clt3.getFrontMatrixIndex(row, col));
            }
        }
    }
    return leds;
}

void wordsLightTheRightLeds() {
    // ES (cols 0-1) IST (cols 3-5) in row 0
    check(ledsOf(FrontWord::es_ist) == std::set<uint16_t>{1, 21, 41, 42, 61},
          "ES IST lights LEDs 1, 21, 41, 42, 61");
    // UHR: row 9, cols 8-10
    check(ledsOf(FrontWord::uhr) == std::set<uint16_t>{91, 92, 112},
          "UHR lights LEDs 91, 92, 112");
}

} // namespace

int main() {
    anchorsFromTheKitWiring();
    minuteLedsAreTheCorners();
    everyLedUsedExactlyOnce();
    wordsLightTheRightLeds();

    return report("clt3 layout");
}
