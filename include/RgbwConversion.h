#pragma once

#include "WordClockState.h"
#include <NeoPixelBus.h>

/*
 * RGB -> RGBW for strips with a white channel.
 *
 * The white LED replaces as much of the requested colour as it can: its own
 * colour (whiteAdj*, per WhiteType) is subtracted from R, G and B, and the
 * remainder is mixed in with the colour LEDs.
 *
 * maximizeOutput: the conversion keeps the requested colour but, for tints
 * the white LED covers well (e.g. warm whites on a warm white strip), leaves
 * every output channel below 255 even at full brightness - the strip could
 * give more light of the same colour. With maximizeOutput all four channels
 * are scaled up together until the largest output channel equals the largest
 * requested channel. The colour stays the same, brightness control stays
 * linear, white and pure colours are unchanged.
 */

namespace rgbw {

const uint8_t whiteAdjR[3] = {255, 255, 255};
const uint8_t whiteAdjG[3] = {180, 215, 107};
const uint8_t whiteAdjB[3] = {107, 177, 253};

inline uint8_t max3(uint8_t a, uint8_t b, uint8_t c) {
    return a > b ? (a > c ? a : c) : (b > c ? b : c);
}

inline RgbwColor convert(RgbColor light, WhiteType wType,
                         bool maximizeOutput = false) {
    RgbwColor returnColor;
    uint8_t wIdx = static_cast<uint8_t>(wType);

    // Use fast 32-bit integer math! (255 * 255 = 65025, easily fits in
    // uint32_t)
    uint32_t whiteValueR = (light.R * 255) / whiteAdjR[wIdx];
    uint32_t whiteValueG = (light.G * 255) / whiteAdjG[wIdx];
    uint32_t whiteValueB = (light.B * 255) / whiteAdjB[wIdx];

    // Find the minimum using integer comparison
    uint32_t minValW = whiteValueR;
    if (whiteValueG < minValW) {
        minValW = whiteValueG;
    }
    if (whiteValueB < minValW) {
        minValW = whiteValueB;
    }
    returnColor.W = (minValW <= 255 ? (uint8_t)minValW : 255);

    // Integer math for the return channels
    returnColor.R =
        static_cast<uint8_t>(light.R - (minValW * whiteAdjR[wIdx]) / 255);
    returnColor.G =
        static_cast<uint8_t>(light.G - (minValW * whiteAdjG[wIdx]) / 255);
    returnColor.B =
        static_cast<uint8_t>(light.B - (minValW * whiteAdjB[wIdx]) / 255);

    if (maximizeOutput) {
        const uint8_t requested = max3(light.R, light.G, light.B);
        uint8_t produced = max3(returnColor.R, returnColor.G, returnColor.B);
        if (returnColor.W > produced) {
            produced = returnColor.W;
        }
        if (produced > 0 && produced < requested) {
            // Round to nearest; the largest channel lands exactly on
            // `requested`, all others stay below it.
            returnColor.R =
                (returnColor.R * requested + produced / 2) / produced;
            returnColor.G =
                (returnColor.G * requested + produced / 2) / produced;
            returnColor.B =
                (returnColor.B * requested + produced / 2) / produced;
            returnColor.W =
                (returnColor.W * requested + produced / 2) / produced;
        }
    }

    return returnColor;
}

} // namespace rgbw
