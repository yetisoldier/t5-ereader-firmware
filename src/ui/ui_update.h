#pragma once

#include <Arduino.h>
#include <vector>
#include "state.h"
#include "../library.h"

// ─── OTA state ──────────────────────────────────────────────────────
enum OtaPhase { OTA_IDLE, OTA_CHECKING, OTA_RESULT, OTA_DOWNLOADING, OTA_DONE, OTA_FAILED };

struct OtaState {
    OtaPhase phase;
    String   latestVersion;
    bool     updateAvailable;
    int      progress;
};

// ─── OTA Screen ─────────────────────────────────────────────────────

// Draw the OTA update screen.
// Handles all OTA phases (checking, result, downloading, done, failed).
// May modify otaState (phase transitions during check).
void ui_ota_draw(OtaState& otaState);

// Handle touch on OTA screen.
// Returns STATE_SETTINGS on back/cancel, STATE_OTA_CHECK otherwise.
// Performs download if update available and tapped.
AppState ui_ota_touch(int x, int y, OtaState& otaState);

// ─── WiFi Upload Screen ────────────────────────────────────────────

// Draw the WiFi upload screen (IP address, QR code).
void ui_wifi_draw();

// Handle touch on WiFi screen.
// Always returns STATE_LIBRARY (any tap exits WiFi mode).
// Stops WiFi upload server, rescans library, clears cover cache.
AppState ui_wifi_touch(int x, int y,
                       std::vector<BookInfo>& books,
                       std::vector<int>& filteredIndices);
