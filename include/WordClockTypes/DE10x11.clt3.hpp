#pragma once

#include "DE10x11.2clock.hpp"

/*
 * Leuchtbildshop CLT3 (QLOCKTWO-compatible 45 cm kit), German front.
 *
 * Same words as Ger10x11Clock - only the wiring differs. The kit's LED strip
 * runs column by column and the four corner (minute) LEDs are spliced INTO the
 * chain instead of being appended at its end:
 *
 *   index 0        corner top left      = minute 1
 *   index 1..10    column 0 (left), top -> bottom
 *   index 11       corner bottom left   = minute 4
 *   index 12..101  columns 1..9, meandering (odd columns bottom -> top)
 *   index 102      corner top right     = minute 2
 *   index 103..112 column 10 (right), top -> bottom
 *   index 113      corner bottom right  = minute 3
 *
 * 114 LEDs in total, one LED per letter. The wiring is fixed by the kit, so the
 * layout variant flags (meander, flip, extra LED per row) and the multi-LED
 * build types are not applied here.
 *
 *           Layout Front
 *       COL 0 1 2 3 4 5 6 7 8 9 X   (getFrontMatrixIndex() columns)
 * ROW + - - - - - - - - - - -
 *  0  | E S K I S T A F Ü N F
 *  1  | Z E H N Z W A N Z I G
 *  2  | D R E I V I E R T E L
 *  3  | V O R F U N K N A C H
 *  4  | H A L B A E L F Ü N F
 *  5  | E I N S X A M Z W E I
 *  6  | D R E I P M J V I E R
 *  7  | S E C H S N L A C H T
 *  8  | S I E B E N Z W Ö L F
 *  9  | Z E H N E U N K U H R
 */

class De10x11Clt3_t : public De10x11Clock_t {
public:
    static constexpr uint16_t CORNER_TOP_LEFT = 0;
    static constexpr uint16_t CORNER_BOTTOM_LEFT = 11;
    static constexpr uint16_t CORNER_TOP_RIGHT = 102;
    static constexpr uint16_t CORNER_BOTTOM_RIGHT = 113;

    //------------------------------------------------------------------------------

    /* row 0..9 top -> bottom, col 0..10 left -> right (display coordinates) */
    uint16_t getFrontMatrixIndex(uint8_t row, uint8_t col) override {
        const uint16_t letter =
            (col % 2 == 0) ? col * 10 + row : col * 10 + 9 - row;
        // Skip the corner LEDs spliced in before letters 0, 10 and 100.
        const uint16_t index =
            letter + (letter < 10 ? 1 : (letter < 100 ? 2 : 3));

        return checkedFrontMatrixIndex(index, CORNER_BOTTOM_RIGHT + 1);
    }

    //------------------------------------------------------------------------------

    /* Minutes 1..4 clockwise from the top left corner. */
    void getMinuteArray(uint16_t *returnArr, MinuteVariant variant) override {
        returnArr[0] = CORNER_TOP_LEFT;
        returnArr[1] = CORNER_TOP_RIGHT;
        returnArr[2] = CORNER_BOTTOM_RIGHT;
        returnArr[3] = CORNER_BOTTOM_LEFT;
    }

    //------------------------------------------------------------------------------

    /* Exactly four corner LEDs, no seconds frame. */
    bool hasLed7x() override { return false; }

    bool supportsSecondsFrame() override { return false; }
};

De10x11Clt3_t _de10x11Clt3;
