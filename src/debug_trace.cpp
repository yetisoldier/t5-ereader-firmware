#include "debug_trace.h"
#include <Preferences.h>
#include <esp_system.h>

static void trace_write(const char* stage, const String& detail) {
    Preferences prefs;
    if (!prefs.begin("ereader", false)) return;
    prefs.putString("dbgStage", stage ? stage : "");
    prefs.putString("dbgDetail", detail);
    prefs.putULong("dbgMs", millis());
    prefs.end();
}

void debug_trace_boot_report() {
    Preferences prefs;
    if (!prefs.begin("ereader", false)) return;
    String stage = prefs.getString("dbgStage", "");
    String detail = prefs.getString("dbgDetail", "");
    unsigned long ms = prefs.getULong("dbgMs", 0);
    int prevResetReason = prefs.getInt("dbgResetReason", -1);

    prefs.putString("dbgPrevStage", stage);
    prefs.putString("dbgPrevDetail", detail);
    prefs.putULong("dbgPrevMs", ms);
    prefs.putInt("dbgPrevReset", prevResetReason);
    prefs.putInt("dbgResetReason", (int)esp_reset_reason());
    prefs.end();

    for (int i = 0; i < 3; ++i) {
        if (stage.length() > 0 || detail.length() > 0) {
            Serial.printf("DBGTRACE last stage=%s detail=%s ms=%lu prev_reset=%d\n",
                          stage.c_str(), detail.c_str(), ms, prevResetReason);
        } else {
            Serial.printf("DBGTRACE last stage=<none> prev_reset=%d\n", prevResetReason);
        }
        delay(120);
    }
}

void debug_trace_mark(const char* stage, const String& detail) {
    trace_write(stage, detail);
    Serial.printf("DBGTRACE mark stage=%s detail=%s\n",
                  stage ? stage : "", detail.c_str());
}

void debug_trace_clear() {
    trace_write("", "");
    Serial.println("DBGTRACE cleared");
}
