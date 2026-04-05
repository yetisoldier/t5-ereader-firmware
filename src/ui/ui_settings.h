#pragma once

#include <Arduino.h>
#include "state.h"

// Forward declaration
class BookReader;

// Draw the settings screen.
// settingsFromLibrary: if true, uses medium refresh instead of full refresh.
void ui_settings_draw(bool& settingsFromLibrary);

// Handle touch on settings screen.
// Returns the new AppState (STATE_LIBRARY, STATE_READER, STATE_WIFI, STATE_OTA_CHECK,
// or STATE_SETTINGS if staying).
// enterSleepCb: callback to trigger deep sleep from settings "Sleep Now" row.
AppState ui_settings_touch(int x, int y, BookReader& reader, void (*enterSleepCb)());
