#pragma once

#include <Arduino.h>
#include <math.h>

// Conservative gamma-style lift for e-paper grayscale conversion.
// Keeps blacks anchored while gently brightening midtones and shadows.
static inline uint8_t image_lift_gray8(uint8_t gray8) {
    static bool lutReady = false;
    static uint8_t lut[256];
    if (!lutReady) {
        // Gamma < 1 brightens the image without washing out highlights.
        // 0.86 is a conservative lift that still preserves contrast.
        constexpr float kGamma = 0.86f;
        for (int i = 0; i < 256; ++i) {
            float n = (float)i / 255.0f;
            float lifted = powf(n, kGamma);
            int v = (int)(lifted * 255.0f + 0.5f);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            lut[i] = (uint8_t)v;
        }
        lutReady = true;
    }
    return lut[gray8];
}

static inline uint8_t image_rgb565_to_gray8(uint16_t px) {
    uint8_t r = ((px >> 11) & 0x1F) << 3;
    uint8_t g = ((px >> 5) & 0x3F) << 2;
    uint8_t b = (px & 0x1F) << 3;
    uint8_t gray8 = (uint8_t)((r * 38 + g * 75 + b * 15) >> 7);
    return image_lift_gray8(gray8);
}

static inline uint8_t image_rgb565_to_gray4(uint16_t px) {
    return image_rgb565_to_gray8(px) >> 4;  // 0=black, 15=white — EPD convention
}

static inline uint8_t image_gray8_to_gray4(uint8_t gray8) {
    return image_lift_gray8(gray8) >> 4;
}
