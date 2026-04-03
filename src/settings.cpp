#include "settings.h"
#include "config.h"
#include "storage_utils.h"
#include <SD.h>
#include <ArduinoJson.h>

static Settings _settings;
static const char* SETTINGS_PATH = "/books/.settings.json";
static const char* SETTINGS_TMP  = "/books/.settings.tmp";

void settings_set_default() {
    _settings.fontSize        = 2;  // legacy: maps to fontSizeLevel 2 (M)
    _settings.fontSizeLevel   = 2;  // M (default)
    _settings.serifFont       = false;
    _settings.sleepTimeoutMin = 5;
    _settings.refreshEveryPages = 4;
    _settings.wifiSSID        = WIFI_SSID;
    _settings.wifiPass        = WIFI_PASS;
    _settings.showPageNumbers = true;
    _settings.showBattery     = true;
    _settings.tapZoneLayout   = 0;
    _settings.libraryViewMode = 0;
    _settings.posterShowCovers = false;
    _settings.opdsActiveServer = 0;
}

void settings_init() {
    settings_set_default();

    File f = SD.open(SETTINGS_PATH, FILE_READ);
    if (!f) {
        Serial.println("Settings: no file found, using defaults");
        settings_save();
        return;
    }

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("Settings: parse error (%s), using defaults\n", err.c_str());
        settings_save();
        return;
    }

    _settings.fontSize        = doc["fontSize"]        | 2;
    _settings.fontSizeLevel   = doc["fontSizeLevel"]   | -1;
    _settings.serifFont       = doc["serifFont"]       | false;
    _settings.sleepTimeoutMin = doc["sleepTimeoutMin"]  | 5;
    _settings.refreshEveryPages = doc["refreshEveryPages"] | 4;
    _settings.wifiSSID        = doc["wifiSSID"]         | WIFI_SSID;
    _settings.wifiPass        = doc["wifiPass"]         | WIFI_PASS;
    _settings.showPageNumbers = doc["showPageNumbers"]  | true;
    _settings.showBattery     = doc["showBattery"]      | true;
    _settings.tapZoneLayout   = doc["tapZoneLayout"]    | 0;
    _settings.libraryViewMode = doc["libraryViewMode"]  | 0;
    _settings.posterShowCovers = doc["posterShowCovers"] | false;
    _settings.opdsActiveServer = doc["opdsActiveServer"] | 0;

    // Migrate old fontSize (0-2) → fontSizeLevel (0-6) if fontSizeLevel wasn't saved
    if (_settings.fontSizeLevel < 0) {
        // Map old 0=small→1(S), 1=medium→2(M), 2=large→4(L)
        const int migration[] = {1, 2, 4};
        int old = _settings.fontSize;
        if (old < 0 || old > 2) old = 1;
        _settings.fontSizeLevel = migration[old];
    }

    // Clamp values
    if (_settings.fontSizeLevel < 0 || _settings.fontSizeLevel > 6) _settings.fontSizeLevel = 2;
    if (_settings.sleepTimeoutMin < 1) _settings.sleepTimeoutMin = 5;
    if (_settings.refreshEveryPages < 1) _settings.refreshEveryPages = 4;
    if (_settings.libraryViewMode < 0 || _settings.libraryViewMode > 1) _settings.libraryViewMode = 0;

    Serial.printf("Settings: loaded (fontLevel=%d, serif=%d, sleep=%dmin, refresh=%d, wifi=%s)\n",
                  _settings.fontSizeLevel, _settings.serifFont,
                  _settings.sleepTimeoutMin,
                  _settings.refreshEveryPages,
                  _settings.wifiSSID.c_str());
}

void settings_save() {
    StaticJsonDocument<512> doc;
    doc["fontSize"]        = _settings.fontSizeLevel; // write new level as fontSize too for compat
    doc["fontSizeLevel"]   = _settings.fontSizeLevel;
    doc["serifFont"]       = _settings.serifFont;
    doc["sleepTimeoutMin"] = _settings.sleepTimeoutMin;
    doc["refreshEveryPages"] = _settings.refreshEveryPages;
    doc["wifiSSID"]        = _settings.wifiSSID;
    doc["wifiPass"]        = _settings.wifiPass;
    doc["showPageNumbers"] = _settings.showPageNumbers;
    doc["showBattery"]     = _settings.showBattery;
    doc["tapZoneLayout"]   = _settings.tapZoneLayout;
    doc["libraryViewMode"] = _settings.libraryViewMode;
    doc["posterShowCovers"] = _settings.posterShowCovers;
    doc["opdsActiveServer"] = _settings.opdsActiveServer;

    String json;
    serializeJson(doc, json);
    if (storage_write_text_atomic(SETTINGS_PATH, SETTINGS_TMP, json)) {
        Serial.println("Settings: saved atomically");
    } else {
        Serial.println("Settings: atomic save failed");
    }
}

Settings& settings_get() {
    return _settings;
}
