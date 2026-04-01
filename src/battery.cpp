#include "battery.h"
#include "config.h"

void battery_init() {
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    pinMode(BATT_PIN, INPUT);
}

float battery_voltage() {
    int raw = analogRead(BATT_PIN);
    // Voltage divider: 2:1 ratio, 3.3V reference, 1.1 correction factor
    return (raw / 4095.0f) * 2.0f * 3.3f * 1.1f;
}

int battery_percent() {
    float v = battery_voltage();
    // LiPo discharge curve approximation (3.0V empty, 4.2V full)
    if (v >= 4.2f) return 100;
    if (v <= 3.0f) return 0;
    // Piecewise linear approximation
    if (v >= 3.9f) return 80 + (int)((v - 3.9f) / 0.3f * 20);
    if (v >= 3.7f) return 40 + (int)((v - 3.7f) / 0.2f * 40);
    return (int)((v - 3.0f) / 0.7f * 40);
}
