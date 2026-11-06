/*
 * PowerLimitedLedStrip: estimates the strip current of each frame from the
 * requested channel values and scales the whole frame down when it would
 * exceed the limit (issue #625).
 *
 * Model: every channel draws milliAmpsPerChannel at 255, linearly.
 */

#include "PowerLimitedLedStrip.h"

#include "support/check.h"

#include <string.h>

namespace {

char message[200];

/* Records what reaches the real strip. */
class FakeStrip : public LedStripInterface {
public:
    static const uint16_t SIZE = 128;
    RgbwColor pixel[SIZE];
    int shows = 0;
    void setRgbPixel(uint16_t index, HsbColor color) override {
        RgbColor rgb(color);
        pixel[index].R = rgb.R;
        pixel[index].G = rgb.G;
        pixel[index].B = rgb.B;
        pixel[index].W = 0;
    }
    void setRgbwPixel(uint16_t index, RgbwColor color) override {
        pixel[index] = color;
    }
    void clearPixel(uint16_t index) override { pixel[index] = RgbwColor(); }
    void show() override { shows++; }

    uint32_t milliAmps(uint16_t count, uint16_t perChannel) const {
        uint32_t sum = 0;
        for (uint16_t i = 0; i < count; i++) {
            sum += pixel[i].R + pixel[i].G + pixel[i].B + pixel[i].W;
        }
        return sum * perChannel / 255;
    }
};

RgbwColor rgbw(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    RgbwColor c;
    c.R = r;
    c.G = g;
    c.B = b;
    c.W = w;
    return c;
}

const uint16_t LEDS = 114;
const uint16_t PER_CHANNEL = 20;

void belowTheLimitNothingChanges() {
    FakeStrip *fake = new FakeStrip();
    PowerLimitedLedStrip strip(fake, LEDS, 4000, PER_CHANNEL);

    // A time display: 25 letters warm white at full W (25 * 20 mA = 500 mA)
    for (uint16_t i = 0; i < 25; i++) {
        strip.setRgbwPixel(i, rgbw(0, 0, 0, 255));
    }
    strip.setRgbwPixel(30, rgbw(12, 34, 56, 78));
    strip.show();

    check(fake->shows == 1, "show() reaches the real strip");
    check(fake->pixel[0].W == 255, "unlimited frame: W stays 255");
    check(fake->pixel[30].R == 12 && fake->pixel[30].G == 34 &&
              fake->pixel[30].B == 56 && fake->pixel[30].W == 78,
          "unlimited frame: colours pass through unchanged");
    check(strip.lastEstimateMilliAmps() ==
              25 * 20 + (12 + 34 + 56 + 78) * 20 / 255,
          "estimate of the frame before limiting");
}

void aboveTheLimitTheFrameIsScaled() {
    FakeStrip *fake = new FakeStrip();
    PowerLimitedLedStrip strip(fake, LEDS, 4000, PER_CHANNEL);

    // Everything full RGBW: 114 * 4 * 20 mA = 9120 mA
    for (uint16_t i = 0; i < LEDS; i++) {
        strip.setRgbwPixel(i, rgbw(255, 255, 255, 255));
    }
    strip.show();

    snprintf(message, sizeof(message), "estimate is 9120 mA (got %u)",
             strip.lastEstimateMilliAmps());
    check(strip.lastEstimateMilliAmps() == 9120, message);

    const uint32_t sent = fake->milliAmps(LEDS, PER_CHANNEL);
    snprintf(message, sizeof(message),
             "frame sent to the strip stays within 4000 mA (got %u)", sent);
    check(sent <= 4000, message);
    snprintf(message, sizeof(message),
             "and is not dimmed more than needed (got %u mA)", sent);
    check(sent >= 3900, message);
}

void scalingKeepsTheColour() {
    FakeStrip *fake = new FakeStrip();
    PowerLimitedLedStrip strip(fake, LEDS, 1000, PER_CHANNEL);

    for (uint16_t i = 0; i < LEDS; i++) {
        strip.setRgbwPixel(i, rgbw(200, 100, 0, 50));
    }
    strip.show();

    const RgbwColor &p = fake->pixel[7];
    check(p.R < 200, "limited frame is darker");
    // Ratios R:G:W = 4:2:1 survive within rounding.
    check(p.R >= 2 * p.G && p.R <= 2 * p.G + 2, "R:G ratio kept");
    check(p.G >= 2 * p.W && p.G <= 2 * p.W + 2, "G:W ratio kept");
    check(p.B == 0, "an unused channel stays off");
}

void limitingDoesNotCompound() {
    FakeStrip *fake = new FakeStrip();
    PowerLimitedLedStrip strip(fake, LEDS, 4000, PER_CHANNEL);

    for (uint16_t i = 0; i < LEDS; i++) {
        strip.setRgbwPixel(i, rgbw(255, 255, 255, 255));
    }
    strip.show();
    const RgbwColor first = fake->pixel[0];
    // Next frame without new input, as a scrolling text or an idle loop
    // would produce: must come out identical, not dimmed again.
    strip.show();
    check(fake->pixel[0].R == first.R && fake->pixel[0].W == first.W,
          "a second show() of the same frame is not dimmed again");
}

void releasingTheLoadRestoresBrightness() {
    FakeStrip *fake = new FakeStrip();
    PowerLimitedLedStrip strip(fake, LEDS, 4000, PER_CHANNEL);

    for (uint16_t i = 0; i < LEDS; i++) {
        strip.setRgbwPixel(i, rgbw(255, 255, 255, 255));
    }
    strip.show();
    for (uint16_t i = 1; i < LEDS; i++) {
        strip.clearPixel(i);
    }
    strip.show();
    check(fake->pixel[0].R == 255 && fake->pixel[0].W == 255,
          "a light frame after a heavy one is at full brightness again");
    check(fake->pixel[1].R == 0 && fake->pixel[1].W == 0,
          "cleared pixels reach the strip as off");
}

void rgbPixelsCountToo() {
    FakeStrip *fake = new FakeStrip();
    PowerLimitedLedStrip strip(fake, LEDS, 600, PER_CHANNEL);

    for (uint16_t i = 0; i < 20; i++) {
        strip.setRgbPixel(i, HsbColor(0.f, 0.f, 1.f)); // white via RGB
    }
    strip.show();
    check(strip.lastEstimateMilliAmps() == 20 * 3 * 20,
          "RGB white counts three channels");
    check(fake->milliAmps(LEDS, PER_CHANNEL) <= 600,
          "RGB frame is limited as well");
}

void outOfRangeIndicesAreIgnored() {
    FakeStrip *fake = new FakeStrip();
    PowerLimitedLedStrip strip(fake, 10, 4000, PER_CHANNEL);
    strip.setRgbwPixel(10, rgbw(255, 255, 255, 255));
    strip.clearPixel(500);
    strip.show();
    check(strip.lastEstimateMilliAmps() == 0,
          "pixels beyond the strip are dropped, like NeoPixelBus does");
}

} // namespace

int main() {
    belowTheLimitNothingChanges();
    aboveTheLimitTheFrameIsScaled();
    scalingKeepsTheColour();
    limitingDoesNotCompound();
    releasingTheLoadRestoresBrightness();
    rgbPixelsCountToo();
    outOfRangeIndicesAreIgnored();

    return report("power limit");
}
