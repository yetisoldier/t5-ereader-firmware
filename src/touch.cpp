#include "touch.h"
#include "config.h"
#include <Wire.h>
#include <TouchDrvGT911.hpp>

static TouchDrvGT911 _touch;
static bool _initialized = false;

bool touch_init() {
    // Wake GT911 from sleep: drive INT pin HIGH before I2C init.
    // Required per LilyGo official example — GT911 may be asleep after power-on.
    if (TOUCH_INT_PIN >= 0) {
        pinMode(TOUCH_INT_PIN, OUTPUT);
        digitalWrite(TOUCH_INT_PIN, HIGH);
        delay(50);
    }

    Wire.begin(TOUCH_SDA, TOUCH_SCL);

    // Scan I2C bus for GT911 at both possible addresses.
    // Address depends on INT pin state at reset — hardware pull-up means it varies.
    uint8_t touchAddr = 0;
    Wire.beginTransmission(0x14);
    if (Wire.endTransmission() == 0) {
        touchAddr = 0x14;
        Serial.println("GT911 found at 0x14");
    }
    Wire.beginTransmission(0x5D);
    if (Wire.endTransmission() == 0) {
        touchAddr = 0x5D;
        Serial.println("GT911 found at 0x5D");
    }

    if (touchAddr == 0) {
        Serial.println("GT911 NOT found on I2C bus!");
        Serial.println("I2C scan:");
        for (uint8_t addr = 1; addr < 127; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                Serial.printf("  Device at 0x%02X\n", addr);
            }
        }
        return false;
    }

    // RST = -1 (hardware pull-up), INT = TOUCH_INT_PIN (GPIO 47)
    _touch.setPins(TOUCH_RST, TOUCH_INT_PIN);

    if (!_touch.begin(Wire, touchAddr, TOUCH_SDA, TOUCH_SCL)) {
        Serial.printf("GT911 begin() failed at address 0x%02X\n", touchAddr);
        return false;
    }

    // Match official LilyGo example coordinate transforms.
    // These produce landscape coordinates (0–960 x, 0–540 y).
    _touch.setMaxCoordinates(PHYS_WIDTH, PHYS_HEIGHT);
    _touch.setSwapXY(true);
    _touch.setMirrorXY(false, true);

    Serial.printf("GT911 initialized at 0x%02X (polling, INT=GPIO%d)\n",
                   touchAddr, TOUCH_INT_PIN);
    _initialized = true;
    return true;
}

bool touch_read(TouchPoint &pt) {
    if (!_initialized) return false;

    if (!_touch.isPressed()) return false;

    int16_t x[2] = {0, 0};
    int16_t y[2] = {0, 0};
    uint8_t count = _touch.getPoint(x, y, 1);
    if (count == 0) return false;

    // Library returns landscape coords after setSwapXY/setMirrorXY.
    // Convert to portrait using inverse of display rotation
    // (display: portrait px,py → landscape lx=py, ly=(PW-1)-px).
    // Inverse: portrait_x = (PW-1) - landscape_y, portrait_y = landscape_x.
    pt.x = (PORTRAIT_W - 1) - y[0];
    pt.y = x[0];
    return true;
}
