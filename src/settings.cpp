#include "settings.h"
#include "config.h"
#include "storage_utils.h"
#include <SD.h>
#include <ArduinoJson.h>

static Settings _settings;
static const char* SETTINGS_PATH = "/books/.settings.json";
static const char* SETTINGS_TMP  = "/books/.settings.tmp";

void settings_set_default() {
    _settings.fontSize        = 1;  // medium
    _settings.sleepTimeoutMin = 5;
    _settings.refreshEveryPages = 4;
    _settings.wifiSSID        = WIFI_SSID;
    _settings.wifiPass        = WIFI_PASS;
    _settings.showPageNumbers = true;
    _settings.showBattery     = true;
    _settings.tapZoneLayout   = 0;
    _settings.libraryViewMode = 0;
    _settings.posterShowCovers = false;
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

    _settings.fontSize        = doc["fontSize"]        | 1;
    _settings.sleepTimeoutMin = doc["sleepTimeoutMin"]  | 5;
    _settings.refreshEveryPages = doc["refreshEveryPages"] | 4;
    _settings.wifiSSID        = doc["wifiSSID"]         | WIFI_SSID;
    _settings.wifiPass        = doc["wifiPass"]         | WIFI_PASS;
    _settings.showPageNumbers = doc["showPageNumbers"]  | true;
    _settings.showBattery     = doc["showBattery"]      | true;
    _settings.tapZoneLayout   = doc["tapZoneLayout"]    | 0;
    _settings.libraryViewMode = doc["libraryViewMode"]  | 0;
    _settings.posterShowCovers = doc["posterShowCovers"] | false;

    // Clamp values
    if (_settings.fontSize < 0 || _settings.fontSize > 2) _settings.fontSize = 1;
    if (_settings.sleepTimeoutMin < 1) _settings.sleepTimeoutMin = 5;
    if (_settings.refreshEveryPages < 1) _settings.refreshEveryPages = 4;
    if (_settings.libraryViewMode < 0 || _settings.libraryViewMode > 1) _settings.libraryViewMode = 0;

    Serial.printf("Settings: loaded (font=%d, sleep=%dmin, refresh=%d, wifi=%s)\n",
                  _settings.fontSize, _settings.sleepTimeoutMin,
                  _settings.refreshEveryPages,
                  _settings.wifiSSID.c_str());
}

void settings_save() {
    StaticJsonDocument<512> doc;
    doc["fontSize"]        = _settings.fontSize;
    doc["sleepTimeoutMin"] = _settings.sleepTimeoutMin;
    doc["refreshEveryPages"] = _settings.refreshEveryPages;
    doc["wifiSSID"]        = _settings.wifiSSID;
    doc["wifiPass"]        = _settings.wifiPass;
    doc["showPageNumbers"] = _settings.showPageNumbers;
    doc["showBattery"]     = _settings.showBattery;
    doc["tapZoneLayout"]   = _settings.tapZoneLayout;
    doc["libraryViewMode"] = _settings.libraryViewMode;
    doc["posterShowCovers"] = _settings.posterShowCovers;

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
