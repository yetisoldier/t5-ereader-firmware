#pragma once

#include <Arduino.h>

struct Settings {
    // Display
    int   fontSize;         // DEPRECATED — mapped to fontSizeLevel on load
    int   fontSizeLevel;    // 0=XS, 1=S, 2=M(default), 3=ML, 4=L, 5=XL, 6=XXL
    bool  serifFont;        // false=sans(FiraSans), true=serif(NotoSerif)
    int   sleepTimeoutMin;  // minutes before deep sleep (default: 5)
    int   refreshEveryPages; // stronger cleanup cadence during reading

    // WiFi
    String wifiSSID;
    String wifiPass;

    // Reading
    bool   showPageNumbers;  // footer page numbers (default: true)
    bool   showBattery;      // battery in header (default: true)
    int    tapZoneLayout;    // 0=left/center/right (default), 1=top/mid/bottom
    int    libraryViewMode;  // 0=list, 1=poster
    bool   posterShowCovers; // experimental: render EPUB cover art in poster view
    int    opdsActiveServer; // index of active OPDS server (default: 0)
};

void settings_init();           // Load from SD or create defaults
void settings_save();           // Write to SD as JSON
Settings& settings_get();       // Reference to live settings
void settings_set_default();    // Reset to factory defaults
