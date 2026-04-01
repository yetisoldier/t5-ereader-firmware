#pragma once

#include <Arduino.h>

struct TouchPoint {
    int16_t x;
    int16_t y;
};

bool touch_init();
bool touch_read(TouchPoint &pt);
