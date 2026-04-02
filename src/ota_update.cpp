#include "ota_update.h"
#include "config.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"

// ─── Internal state ────────────────────────────────────────────────
static String _downloadUrl;
static size_t _downloadSize = 0;

// ─── Version comparison ────────────────────────────────────────────

static bool parseVersion(const char* s, int& major, int& minor, int& patch) {
    // Accept optional leading 'v'
    if (s[0] == 'v' || s[0] == 'V') s++;
    return sscanf(s, "%d.%d.%d", &major, &minor, &patch) == 3;
}

bool ota_is_update_newer(const String& latestTag) {
    int cMaj = 0, cMin = 0, cPat = 0;
    int nMaj = 0, nMin = 0, nPat = 0;

    if (!parseVersion(FIRMWARE_VERSION, cMaj, cMin, cPat)) return false;
    if (!parseVersion(latestTag.c_str(), nMaj, nMin, nPat)) return false;

    if (nMaj != cMaj) return nMaj > cMaj;
    if (nMin != cMin) return nMin > cMin;
    return nPat > cPat;
}

// ─── GitHub release check ──────────────────────────────────────────

bool ota_check_for_update(String& latestVersion) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("OTA: WiFi not connected");
        return false;
    }

    HTTPClient http;
    http.setUserAgent(String("T5EReader-ESP32-") + FIRMWARE_VERSION);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000);

    if (!http.begin("https://api.github.com/repos/yetisoldier/t5-ereader-firmware/releases/latest")) {
        Serial.println("OTA: HTTP begin failed");
        return false;
    }

    int code = http.GET();
    if (code != 200) {
        Serial.printf("OTA: GitHub API returned %d\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    // Parse JSON
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("OTA: JSON parse error: %s\n", err.c_str());
        return false;
    }

    latestVersion = doc["tag_name"].as<String>();
    if (latestVersion.length() == 0) {
        Serial.println("OTA: no tag_name in response");
        return false;
    }

    // Find firmware.bin in assets
    _downloadUrl = "";
    _downloadSize = 0;
    JsonArray assets = doc["assets"].as<JsonArray>();
    for (JsonObject asset : assets) {
        String name = asset["name"].as<String>();
        if (name == "firmware.bin") {
            _downloadUrl = asset["browser_download_url"].as<String>();
            _downloadSize = asset["size"] | 0;
            break;
        }
    }

    if (_downloadUrl.length() == 0) {
        Serial.println("OTA: firmware.bin not found in assets");
        return false;
    }

    Serial.printf("OTA: latest=%s, current=%s, url=%s, size=%u\n",
                  latestVersion.c_str(), FIRMWARE_VERSION,
                  _downloadUrl.c_str(), _downloadSize);

    return ota_is_update_newer(latestVersion);
}

// ─── OTA install via esp_https_ota ─────────────────────────────────

bool ota_install_update(std::function<void(int)> progressCallback) {
    if (_downloadUrl.length() == 0) {
        Serial.println("OTA: no download URL (call checkForUpdate first)");
        return false;
    }

    Serial.printf("OTA: downloading from %s\n", _downloadUrl.c_str());

    // Disable WiFi power saving for reliable download
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Follow the GitHub redirect to get the actual download URL.
    // GitHub releases use 302 redirects to S3-backed storage.
    HTTPClient redirectHttp;
    redirectHttp.setUserAgent(String("T5EReader-ESP32-") + FIRMWARE_VERSION);
    redirectHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    redirectHttp.setTimeout(30000);
    redirectHttp.begin(_downloadUrl);
    // We use collectHeaders to capture the final URL after redirects
    const char* headerKeys[] = {"Location"};
    redirectHttp.collectHeaders(headerKeys, 1);

    // Do a HEAD to resolve redirect chain without downloading the binary
    int code = redirectHttp.sendRequest("HEAD");
    String finalUrl = _downloadUrl;

    // HTTPClient with STRICT_FOLLOW_REDIRECTS follows automatically,
    // so by the time we get a 200 the URL in the client is the final one.
    // But esp_https_ota needs the direct URL. Re-resolve manually:
    if (code == 200 || code == 302) {
        // For the actual download, we'll use the original URL with esp_https_ota
        // and let it handle redirects via the HTTP client internally.
        // Actually, esp_https_ota doesn't follow redirects well, so we need
        // the final URL. Let's use HTTPClient GET with streaming to get the
        // Location after redirect.
    }
    redirectHttp.end();

    // Use HTTPClient to resolve the final URL by following redirects
    HTTPClient resolveHttp;
    resolveHttp.setUserAgent(String("T5EReader-ESP32-") + FIRMWARE_VERSION);
    resolveHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    resolveHttp.setTimeout(30000);
    resolveHttp.begin(_downloadUrl);

    code = resolveHttp.GET();
    if (code != 200) {
        Serial.printf("OTA: download GET returned %d\n", code);
        resolveHttp.end();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    int contentLength = resolveHttp.getSize();
    if (contentLength <= 0 && _downloadSize > 0) {
        contentLength = _downloadSize;
    }

    WiFiClient* stream = resolveHttp.getStreamPtr();
    if (!stream) {
        Serial.println("OTA: failed to get stream");
        resolveHttp.end();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    // Use ESP-IDF OTA API directly with the stream
    esp_ota_handle_t otaHandle = 0;
    const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(NULL);
    if (!updatePartition) {
        Serial.println("OTA: no update partition found");
        resolveHttp.end();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    Serial.printf("OTA: writing to partition '%s' at 0x%x, size %d\n",
                  updatePartition->label, updatePartition->address, contentLength);

    esp_err_t err = esp_ota_begin(updatePartition, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle);
    if (err != ESP_OK) {
        Serial.printf("OTA: esp_ota_begin failed: %s\n", esp_err_to_name(err));
        resolveHttp.end();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    // Read and write in chunks
    const size_t bufSize = 4096;
    uint8_t* buf = (uint8_t*)malloc(bufSize);
    if (!buf) {
        Serial.println("OTA: malloc failed");
        esp_ota_abort(otaHandle);
        resolveHttp.end();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    size_t written = 0;
    int lastPercent = -1;
    bool success = true;
    unsigned long lastDataTime = millis();

    while (written < (size_t)contentLength || contentLength <= 0) {
        // Check for timeout (30s with no data)
        if (millis() - lastDataTime > 30000) {
            Serial.println("OTA: download timeout");
            success = false;
            break;
        }

        size_t available = stream->available();
        if (available == 0) {
            // Check if connection is still alive
            if (!stream->connected() && stream->available() == 0) {
                if (contentLength > 0 && written < (size_t)contentLength) {
                    Serial.println("OTA: connection lost prematurely");
                    success = false;
                }
                break;
            }
            delay(10);
            continue;
        }

        lastDataTime = millis();
        size_t toRead = (available < bufSize) ? available : bufSize;
        if (contentLength > 0 && (written + toRead) > (size_t)contentLength) {
            toRead = contentLength - written;
        }

        int bytesRead = stream->readBytes(buf, toRead);
        if (bytesRead <= 0) {
            delay(10);
            continue;
        }

        err = esp_ota_write(otaHandle, buf, bytesRead);
        if (err != ESP_OK) {
            Serial.printf("OTA: write failed at %u: %s\n", written, esp_err_to_name(err));
            success = false;
            break;
        }

        written += bytesRead;

        if (contentLength > 0 && progressCallback) {
            int pct = (int)(written * 100 / contentLength);
            if (pct != lastPercent) {
                lastPercent = pct;
                progressCallback(pct);
            }
        }
    }

    free(buf);
    resolveHttp.end();

    if (!success || (contentLength > 0 && written != (size_t)contentLength)) {
        Serial.printf("OTA: download incomplete (%u / %d)\n", written, contentLength);
        esp_ota_abort(otaHandle);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    err = esp_ota_end(otaHandle);
    if (err != ESP_OK) {
        Serial.printf("OTA: esp_ota_end failed: %s\n", esp_err_to_name(err));
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    err = esp_ota_set_boot_partition(updatePartition);
    if (err != ESP_OK) {
        Serial.printf("OTA: set boot partition failed: %s\n", esp_err_to_name(err));
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    // Re-enable power saving
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    Serial.printf("OTA: success! Wrote %u bytes to '%s'\n", written, updatePartition->label);
    return true;
}
