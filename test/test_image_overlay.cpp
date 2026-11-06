/*
 * ImageOverlay: pictures and animations sent over MQTT (image/set), shown on
 * top of the current mode and ended by loops, duration or image/clear.
 */

#include "ImageOverlay.h"

#include "support/check.h"

#include <string>

namespace {

char message[200];

std::string hexFrame(uint16_t pixels, const char *rgb) {
    std::string s;
    for (uint16_t i = 0; i < pixels; i++) {
        s += rgb;
    }
    return s;
}

void hexPixelsAreValidated() {
    RgbColor out[3];
    check(image::parseHexPixels("FF000000ff00a0B0c0", 3, out),
          "upper and lower case hex is accepted");
    check(out[0].R == 255 && out[0].G == 0 && out[0].B == 0, "pixel 0 red");
    check(out[1].G == 255, "pixel 1 green");
    check(out[2].R == 0xA0 && out[2].G == 0xB0 && out[2].B == 0xC0,
          "pixel 2 A0B0C0");
    check(!image::parseHexPixels("FF0000", 3, out), "too short is rejected");
    check(!image::parseHexPixels("FF000000FF000000FF00", 3, out),
          "too long is rejected");
    check(!image::parseHexPixels("FF0000GG0000000000", 3, out),
          "non-hex digits are rejected");
    check(!image::parseHexPixels(nullptr, 3, out), "missing text is rejected");
}

void aStillImageStaysUntilCleared() {
    ImageOverlay overlay(110, 4, 8);
    check(!overlay.active(), "idle at start");

    check(overlay.begin(1, 0, 0, 255) == image::Result::Ok, "begin");
    check(overlay.setFrame(0, hexFrame(114, "FF0000").c_str(), 0) ==
              image::Result::Ok,
          "a frame with 110 letters + 4 corners is accepted");
    check(overlay.commit(1000) == image::Result::Ok, "commit");
    check(overlay.active(), "active after commit");

    bool changed = false;
    const RgbColor *frame = overlay.update(1000, changed);
    check(frame != nullptr && changed, "first update draws the frame");
    check(frame[113].R == 255, "corner pixels are part of the frame");
    overlay.update(500000, changed);
    check(overlay.active() && !changed,
          "no loops, no duration: still shown much later, nothing to redraw");

    overlay.clear();
    check(!overlay.active(), "clear() ends it");
}

void lettersOnlyIsAccepted() {
    ImageOverlay overlay(110, 4, 8);
    overlay.begin(1, 0, 0, 255);
    check(overlay.setFrame(0, hexFrame(110, "00FF00").c_str(), 0) ==
              image::Result::Ok,
          "a frame with only the 110 letters is accepted");
    overlay.commit(0);
    bool changed;
    const RgbColor *frame = overlay.update(0, changed);
    check(frame[109].G == 255, "letters set");
    check(frame[110].R == 0 && frame[110].G == 0 && frame[113].B == 0,
          "corners stay off");
}

void framesAdvanceByTheirOwnTime() {
    ImageOverlay overlay(110, 4, 8);
    overlay.begin(3, 0, 0, 255);
    overlay.setFrame(0, hexFrame(110, "010000").c_str(), 100);
    overlay.setFrame(1, hexFrame(110, "020000").c_str(), 300);
    overlay.setFrame(2, hexFrame(110, "030000").c_str(), 50);
    overlay.commit(1000);

    bool changed;
    check(overlay.update(1000, changed)[0].R == 1 && changed, "t=0: frame 0");
    check(overlay.update(1099, changed)[0].R == 1 && !changed,
          "t=99: still frame 0");
    check(overlay.update(1100, changed)[0].R == 2 && changed, "t=100: frame 1");
    check(overlay.update(1399, changed)[0].R == 2, "t=399: still frame 1");
    check(overlay.update(1400, changed)[0].R == 3, "t=400: frame 2");
    check(overlay.update(1450, changed)[0].R == 1,
          "t=450: endless loop wraps to frame 0");
}

void loopsEndTheAnimation() {
    ImageOverlay overlay(110, 4, 8);
    overlay.begin(2, 2, 0, 255); // 2 frames, 2 loops
    overlay.setFrame(0, hexFrame(110, "010000").c_str(), 100);
    overlay.setFrame(1, hexFrame(110, "020000").c_str(), 100);
    overlay.commit(0);

    bool changed;
    overlay.update(0, changed);
    overlay.update(399, changed);
    check(overlay.active(), "second loop still running at t=399");
    const RgbColor *frame = overlay.update(400, changed);
    check(frame == nullptr && !overlay.active(),
          "after 2 loops (400 ms) the overlay ends");
}

void durationEndsIt() {
    ImageOverlay overlay(110, 4, 8);
    overlay.begin(1, 0, 5, 255); // 5 s
    overlay.setFrame(0, hexFrame(110, "FFFFFF").c_str(), 0);
    overlay.commit(10000);
    bool changed;
    overlay.update(14999, changed);
    check(overlay.active(), "4.999 s: still shown");
    check(overlay.update(15000, changed) == nullptr && !overlay.active(),
          "5 s: ended");
}

void millisWrapAround() {
    ImageOverlay overlay(110, 4, 8);
    overlay.begin(1, 0, 2, 255);
    overlay.setFrame(0, hexFrame(110, "FFFFFF").c_str(), 0);
    overlay.commit(0xFFFFFC18UL); // 1 s before millis() wraps
    bool changed;
    overlay.update(500, changed);
    check(overlay.active(), "1.5 s across the wrap: still shown");
    overlay.update(1000, changed);
    check(!overlay.active(), "2 s across the wrap: ended");
}

void limitsAreEnforced() {
    ImageOverlay overlay(110, 4, 8);
    check(overlay.begin(9, 0, 0, 255) == image::Result::TooManyFrames,
          "more frames than maxFrames is rejected");
    check(overlay.begin(0, 0, 0, 255) == image::Result::NoFrames,
          "zero frames is rejected");
    check(!overlay.active(), "a rejected message leaves nothing active");

    overlay.begin(2, 0, 0, 255);
    check(overlay.setFrame(0, hexFrame(111, "FFFFFF").c_str(), 0) ==
              image::Result::BadPixels,
          "111 pixels is neither letters only nor letters + corners");
    check(overlay.setFrame(5, hexFrame(110, "FFFFFF").c_str(), 0) ==
              image::Result::BadFrameIndex,
          "frame index beyond the announced count");
    overlay.setFrame(0, hexFrame(110, "FFFFFF").c_str(), 0);
    check(overlay.commit(0) == image::Result::MissingFrame,
          "commit with a frame left out is rejected");
    check(!overlay.active(), "and nothing is shown");
}

void aNewImageReplacesTheRunningOne() {
    ImageOverlay overlay(110, 4, 8);
    overlay.begin(1, 0, 0, 255);
    overlay.setFrame(0, hexFrame(110, "FF0000").c_str(), 0);
    overlay.commit(0);

    // A broken message must not kill the image that is showing.
    overlay.begin(1, 0, 0, 255);
    overlay.setFrame(0, "nonsense", 0);
    check(overlay.commit(10) != image::Result::Ok, "broken message refused");
    bool changed;
    const RgbColor *frame = overlay.update(20, changed);
    check(overlay.active() && frame != nullptr && frame[0].R == 255,
          "the previous image keeps running");

    overlay.begin(1, 0, 0, 255);
    overlay.setFrame(0, hexFrame(110, "0000FF").c_str(), 0);
    overlay.commit(30);
    frame = overlay.update(30, changed);
    check(changed && frame[0].B == 255, "a valid new image replaces it");
}

void stopEndsAtTheNextUpdate() {
    ImageOverlay overlay(110, 4, 8);
    overlay.begin(1, 0, 0, 255);
    overlay.setFrame(0, hexFrame(110, "FF0000").c_str(), 0);
    overlay.commit(0);
    bool changed;
    overlay.update(0, changed);
    overlay.stop();
    check(overlay.active(),
          "after stop() it is still active, so the caller sees the end");
    check(overlay.update(10, changed) == nullptr && !overlay.active(),
          "the next update() ends it");
    overlay.stop();
    check(!overlay.active(), "stop() without an image does nothing");

    overlay.begin(1, 0, 0, 255);
    overlay.setFrame(0, hexFrame(110, "00FF00").c_str(), 0);
    overlay.commit(20);
    check(overlay.update(20, changed) != nullptr,
          "a new image after a stop is shown normally");
}

void brightnessScalesTheColours() {
    ImageOverlay overlay(110, 4, 8);
    overlay.begin(1, 0, 0, 128); // ~50 %
    overlay.setFrame(0, hexFrame(110, "FF8040").c_str(), 0);
    overlay.commit(0);
    bool changed;
    const RgbColor *frame = overlay.update(0, changed);
    snprintf(message, sizeof(message), "FF8040 at 128/255 -> %02X%02X%02X",
             frame[0].R, frame[0].G, frame[0].B);
    check(frame[0].R == 128 && frame[0].G == 64 && frame[0].B == 32, message);
}

} // namespace

int main() {
    hexPixelsAreValidated();
    aStillImageStaysUntilCleared();
    lettersOnlyIsAccepted();
    framesAdvanceByTheirOwnTime();
    loopsEndTheAnimation();
    durationEndsIt();
    millisWrapAround();
    limitsAreEnforced();
    aNewImageReplacesTheRunningOne();
    stopEndsAtTheNextUpdate();
    brightnessScalesTheColours();

    return report("image overlay");
}
