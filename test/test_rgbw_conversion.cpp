/*
 * rgbw::convert(): RGB -> RGBW, with and without maximizeOutput.
 *
 * Without the option the conversion must stay bit-identical to the version
 * that lived in Led.cpp (reference copy below). With it, warm tints on a
 * warm white strip get more light of the same colour.
 */

#include "RgbwConversion.h"

#include "support/check.h"

#include <stdlib.h>

namespace {

char message[200];

/* Verbatim copy of the conversion as it was in src/Led.cpp. */
RgbwColor reference(RgbColor light, WhiteType wType) {
    const uint8_t adjR[3] = {255, 255, 255};
    const uint8_t adjG[3] = {180, 215, 107};
    const uint8_t adjB[3] = {107, 177, 253};
    RgbwColor out;
    uint8_t w = static_cast<uint8_t>(wType);
    uint32_t r = (light.R * 255) / adjR[w];
    uint32_t g = (light.G * 255) / adjG[w];
    uint32_t b = (light.B * 255) / adjB[w];
    uint32_t m = r < g ? (r < b ? r : b) : (g < b ? g : b);
    out.W = (m <= 255 ? (uint8_t)m : 255);
    out.R = static_cast<uint8_t>(light.R - (m * adjR[w]) / 255);
    out.G = static_cast<uint8_t>(light.G - (m * adjG[w]) / 255);
    out.B = static_cast<uint8_t>(light.B - (m * adjB[w]) / 255);
    return out;
}

bool same(const RgbwColor &a, const RgbwColor &b) {
    return a.R == b.R && a.G == b.G && a.B == b.B && a.W == b.W;
}

uint8_t maxOf(const RgbwColor &c) {
    uint8_t m = c.R > c.G ? c.R : c.G;
    m = m > c.B ? m : c.B;
    return m > c.W ? m : c.W;
}

const WhiteType TYPES[] = {WhiteType::WarmWhite, WhiteType::NeutralWhite,
                           WhiteType::ColdWhite};

void defaultIsUnchanged() {
    unsigned mismatches = 0;
    for (WhiteType t : TYPES) {
        for (int r = 0; r < 256; r += 5) {
            for (int g = 0; g < 256; g += 5) {
                for (int b = 0; b < 256; b += 5) {
                    RgbColor c(r, g, b);
                    mismatches += !same(rgbw::convert(c, t), reference(c, t));
                }
            }
        }
    }
    snprintf(message, sizeof(message),
             "without maximizeOutput the result is unchanged (%u mismatches)",
             mismatches);
    check(mismatches == 0, message);
}

void warmWhiteGetsMoreLight() {
    // Hue 28, saturation 81 %, full brightness: the 2537 K mix from
    // 05-weissmischung-und-cct.md.
    RgbColor warm(HsbColor(28.f / 360.f, 0.81f, 1.f));
    RgbwColor plain = rgbw::convert(warm, WhiteType::WarmWhite);
    RgbwColor max = rgbw::convert(warm, WhiteType::WarmWhite, true);

    snprintf(message, sizeof(message),
             "plain conversion leaves headroom (max channel %u)", maxOf(plain));
    check(maxOf(plain) < 200, message);
    check(maxOf(max) == 255, "maximized: the largest channel reaches 255");

    // Same colour: every channel scaled by the same factor (within rounding).
    const float f = 255.f / maxOf(plain);
    snprintf(message, sizeof(message),
             "same ratios: R %u->%u G %u->%u B %u->%u W %u->%u", plain.R, max.R,
             plain.G, max.G, plain.B, max.B, plain.W, max.W);
    check(abs(max.R - (int)(plain.R * f + 0.5f)) <= 1 &&
              abs(max.G - (int)(plain.G * f + 0.5f)) <= 1 &&
              abs(max.B - (int)(plain.B * f + 0.5f)) <= 1 &&
              abs(max.W - (int)(plain.W * f + 0.5f)) <= 1,
          message);
}

void whiteAndPureColoursUnchanged() {
    const RgbColor colours[] = {RgbColor(255, 255, 255), RgbColor(255, 0, 0),
                                RgbColor(0, 255, 0),     RgbColor(0, 0, 255),
                                RgbColor(255, 128, 0),   RgbColor(0, 0, 0)};
    for (WhiteType t : TYPES) {
        for (const RgbColor &c : colours) {
            snprintf(message, sizeof(message),
                     "(%u,%u,%u) type %u is unchanged by maximizeOutput", c.R,
                     c.G, c.B, static_cast<unsigned>(t));
            check(same(rgbw::convert(c, t, true), rgbw::convert(c, t)),
                  message);
        }
    }
}

void brightnessStaysLinearAndBounded() {
    unsigned violations = 0;
    for (WhiteType t : TYPES) {
        for (int r = 0; r < 256; r += 3) {
            for (int g = 0; g < 256; g += 7) {
                for (int b = 0; b < 256; b += 11) {
                    RgbColor c(r, g, b);
                    RgbwColor out = rgbw::convert(c, t, true);
                    const uint8_t requested = rgbw::max3(c.R, c.G, c.B);
                    // Never brighter than requested, never darker than the
                    // plain conversion.
                    violations += maxOf(out) > requested;
                    violations += maxOf(out) < maxOf(rgbw::convert(c, t));
                }
            }
        }
    }
    snprintf(message, sizeof(message),
             "maximized output stays between plain and requested (%u "
             "violations)",
             violations);
    check(violations == 0, message);

    // Dimming the request dims the result proportionally.
    RgbColor warm(HsbColor(28.f / 360.f, 0.81f, 1.f));
    RgbColor half(HsbColor(28.f / 360.f, 0.81f, 0.5f));
    const uint8_t full = maxOf(rgbw::convert(warm, WhiteType::WarmWhite, true));
    const uint8_t dim = maxOf(rgbw::convert(half, WhiteType::WarmWhite, true));
    snprintf(message, sizeof(message),
             "half brightness gives half output (%u vs %u)", dim, full);
    check(abs(2 * dim - full) <= 3, message);
}

} // namespace

int main() {
    defaultIsUnchanged();
    warmWhiteGetsMoreLight();
    whiteAndPureColoursUnchanged();
    brightnessStaysLinearAndBounded();

    return report("rgbw conversion");
}
