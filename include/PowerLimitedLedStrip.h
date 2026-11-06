#pragma once

#include "LedStripInterface.h"

/*
 * Keeps the LED strip within a current budget (issue #625).
 *
 * Wraps the real strip and remembers the colour every pixel was asked for.
 * On show() it estimates the frame's current from those values - each channel
 * draws milliAmpsPerChannel at 255, linearly - and, if the estimate exceeds
 * the limit, scales every channel of every pixel by the same factor before
 * handing the frame to the real strip. Colours keep their ratios; frames below
 * the limit pass through unchanged.
 *
 * The requested values are kept unscaled, so a frame that is shown again
 * (scrolling text, idle loops) is not dimmed a second time, and a lighter
 * frame after a heavy one is at full brightness again.
 *
 * The estimate covers the LED channels only. Leave headroom for the
 * controller and the LEDs' idle current when choosing the limit, e.g. 4000 mA
 * on a 5 A supply.
 */
class PowerLimitedLedStrip : public LedStripInterface {
public:
    PowerLimitedLedStrip(LedStripInterface *inner, uint16_t pixelCount,
                         uint32_t limitMilliAmps, uint16_t milliAmpsPerChannel)
        : m_inner(inner), m_pixelCount(pixelCount),
          m_limitMilliAmps(limitMilliAmps),
          m_milliAmpsPerChannel(milliAmpsPerChannel),
          m_requested(new RgbwColor[pixelCount]) {
        for (uint16_t i = 0; i < m_pixelCount; i++) {
            m_requested[i] = off();
        }
    }

    ~PowerLimitedLedStrip() override {
        delete[] m_requested;
        delete m_inner;
    }

    PowerLimitedLedStrip(const PowerLimitedLedStrip &) = delete;
    PowerLimitedLedStrip &operator=(const PowerLimitedLedStrip &) = delete;

    void setRgbPixel(uint16_t index, HsbColor color) override {
        if (index >= m_pixelCount) {
            return;
        }
        RgbColor rgb(color);
        m_requested[index].R = rgb.R;
        m_requested[index].G = rgb.G;
        m_requested[index].B = rgb.B;
        m_requested[index].W = 0;
    }

    void setRgbwPixel(uint16_t index, RgbwColor color) override {
        if (index < m_pixelCount) {
            m_requested[index] = color;
        }
    }

    void clearPixel(uint16_t index) override {
        if (index < m_pixelCount) {
            m_requested[index] = off();
        }
    }

    void show() override {
        uint32_t channelSum = 0;
        for (uint16_t i = 0; i < m_pixelCount; i++) {
            const RgbwColor &c = m_requested[i];
            channelSum += c.R + c.G + c.B + c.W;
        }

        // mA = channelSum / 255 * milliAmpsPerChannel
        const uint64_t demand =
            static_cast<uint64_t>(channelSum) * m_milliAmpsPerChannel;
        const uint64_t budget = static_cast<uint64_t>(m_limitMilliAmps) * 255;
        m_lastEstimateMilliAmps = static_cast<uint32_t>(demand / 255);

        const bool limit = m_limitMilliAmps > 0 && demand > budget;
        for (uint16_t i = 0; i < m_pixelCount; i++) {
            const RgbwColor &c = m_requested[i];
            if (!limit) {
                m_inner->setRgbwPixel(i, c);
                continue;
            }
            RgbwColor scaled;
            scaled.R = scale(c.R, budget, demand);
            scaled.G = scale(c.G, budget, demand);
            scaled.B = scale(c.B, budget, demand);
            scaled.W = scale(c.W, budget, demand);
            m_inner->setRgbwPixel(i, scaled);
        }
        m_inner->show();
    }

    /* Estimated current of the last frame before limiting, in mA. */
    uint32_t lastEstimateMilliAmps() const { return m_lastEstimateMilliAmps; }

private:
    static RgbwColor off() {
        RgbwColor c;
        c.R = c.G = c.B = c.W = 0;
        return c;
    }

    // Rounds down, so the scaled frame never exceeds the budget.
    static uint8_t scale(uint8_t value, uint64_t budget, uint64_t demand) {
        return static_cast<uint8_t>(value * budget / demand);
    }

    LedStripInterface *m_inner;
    uint16_t m_pixelCount;
    uint32_t m_limitMilliAmps;
    uint16_t m_milliAmpsPerChannel;
    RgbwColor *m_requested;
    uint32_t m_lastEstimateMilliAmps = 0;
};
