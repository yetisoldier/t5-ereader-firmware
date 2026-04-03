#include "opds_client.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <tinyxml2.h>

using namespace tinyxml2;

// ─── URL resolution ─────────────────────────────────────────────────

String OpdsClient::resolveUrl(const String& base, const String& relative) {
    if (relative.startsWith("http://") || relative.startsWith("https://")) {
        return relative;
    }

    // Extract scheme + host from base
    int schemeEnd = base.indexOf("://");
    if (schemeEnd < 0) return relative;
    int hostEnd = base.indexOf('/', schemeEnd + 3);

    if (relative.startsWith("/")) {
        // Absolute path
        if (hostEnd < 0) return base + relative;
        return base.substring(0, hostEnd) + relative;
    }

    // Relative path — append to base directory
    int lastSlash = base.lastIndexOf('/');
    if (lastSlash <= schemeEnd + 2) {
        return base + "/" + relative;
    }
    return base.substring(0, lastSlash + 1) + relative;
}

// ─── Fetch OPDS catalog ─────────────────────────────────────────────

std::vector<OpdsEntry> OpdsClient::fetchCatalog(const String& url,
                                                  const String& username,
                                                  const String& password) {
    std::vector<OpdsEntry> entries;
    _nextPageUrl = "";
    _lastError = "";

    if (WiFi.status() != WL_CONNECTED) {
        _lastError = "WiFi not connected";
        return entries;
    }

    HTTPClient http;
    http.begin(url);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.addHeader("Accept", "application/atom+xml,application/xml,text/xml");

    if (username.length() > 0) {
        http.setAuthorization(username.c_str(), password.c_str());
    }

    int httpCode = http.GET();
    if (httpCode != 200) {
        _lastError = "HTTP " + String(httpCode);
        http.end();
        return entries;
    }

    String payload = http.getString();
    http.end();

    Serial.printf("OPDS: fetched %d bytes from %s\n", payload.length(), url.c_str());

    // Parse XML
    XMLDocument doc;
    XMLError err = doc.Parse(payload.c_str(), payload.length());
    if (err != XML_SUCCESS) {
        _lastError = "XML parse error";
        return entries;
    }

    XMLElement* feed = doc.FirstChildElement("feed");
    if (!feed) {
        _lastError = "No <feed> element";
        return entries;
    }

    // Check for next page link
    for (XMLElement* link = feed->FirstChildElement("link");
         link; link = link->NextSiblingElement("link")) {
        const char* rel = link->Attribute("rel");
        const char* href = link->Attribute("href");
        if (rel && href && strcmp(rel, "next") == 0) {
            _nextPageUrl = resolveUrl(url, String(href));
        }
    }

    // Parse entries
    for (XMLElement* entry = feed->FirstChildElement("entry");
         entry; entry = entry->NextSiblingElement("entry")) {
        OpdsEntry e;
        e.isNavigation = false;

        // Title
        XMLElement* titleEl = entry->FirstChildElement("title");
        if (titleEl && titleEl->GetText()) {
            e.title = String(titleEl->GetText());
        }

        // Author
        XMLElement* authorEl = entry->FirstChildElement("author");
        if (authorEl) {
            XMLElement* nameEl = authorEl->FirstChildElement("name");
            if (nameEl && nameEl->GetText()) {
                e.author = String(nameEl->GetText());
            }
        }

        // Links — look for acquisition and navigation
        for (XMLElement* link = entry->FirstChildElement("link");
             link; link = link->NextSiblingElement("link")) {
            const char* rel = link->Attribute("rel");
            const char* href = link->Attribute("href");
            const char* type = link->Attribute("type");

            if (!href) continue;
            String hrefStr = resolveUrl(url, String(href));

            // Check for EPUB acquisition link
            if (rel && strstr(rel, "http://opds-spec.org/acquisition") != nullptr) {
                if (type && strcmp(type, EPUB_MIME) == 0) {
                    e.acquisitionUrl = hrefStr;
                    e.mimeType = EPUB_MIME;
                }
            }

            // Check for navigation/catalog links
            if (type && (strstr(type, "application/atom+xml") != nullptr ||
                         strstr(type, "navigation") != nullptr ||
                         strstr(type, "catalog") != nullptr)) {
                if (rel == nullptr ||
                    strstr(rel, "subsection") != nullptr ||
                    strstr(rel, "http://opds-spec.org/sort") != nullptr ||
                    strcmp(rel, "alternate") == 0) {
                    e.navUrl = hrefStr;
                    e.isNavigation = true;
                }
            }

            // Fallback: detail/entry link for nested resolution
            if (!e.detailUrl.length() && rel && strcmp(rel, "alternate") == 0 && type &&
                strstr(type, "application/atom+xml") != nullptr) {
                e.detailUrl = hrefStr;
            }
        }

        // If entry has no acquisition URL but has a navigation URL, mark as navigation
        if (e.acquisitionUrl.length() == 0 && e.navUrl.length() > 0) {
            e.isNavigation = true;
        }

        // Only include entries that have either an EPUB link or a navigation link
        if (e.acquisitionUrl.length() > 0 || e.isNavigation) {
            entries.push_back(e);
        }
    }

    Serial.printf("OPDS: parsed %d entries\n", entries.size());
    return entries;
}

// ─── Resolve EPUB URL (walk nested feeds) ───────────────────────────

String OpdsClient::resolveEpubUrl(const String& detailUrl,
                                   const String& username,
                                   const String& password,
                                   int maxDepth) {
    String currentUrl = detailUrl;

    for (int depth = 0; depth < maxDepth; depth++) {
        auto entries = fetchCatalog(currentUrl, username, password);

        for (const auto& e : entries) {
            if (e.acquisitionUrl.length() > 0 &&
                e.mimeType == EPUB_MIME) {
                return e.acquisitionUrl;
            }
        }

        // Try to drill into the first navigation entry
        bool foundNav = false;
        for (const auto& e : entries) {
            if (e.isNavigation && e.navUrl.length() > 0) {
                currentUrl = e.navUrl;
                foundNav = true;
                break;
            }
        }

        if (!foundNav) break;
    }

    return "";
}

// ─── Download EPUB ──────────────────────────────────────────────────

bool OpdsClient::downloadEpub(const String& url,
                               const String& targetPath,
                               const String& username,
                               const String& password,
                               std::function<void(int pct)> onProgress) {
    _lastError = "";

    if (WiFi.status() != WL_CONNECTED) {
        _lastError = "WiFi not connected";
        return false;
    }

    HTTPClient http;
    http.begin(url);
    http.setTimeout(30000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (username.length() > 0) {
        http.setAuthorization(username.c_str(), password.c_str());
    }

    int httpCode = http.GET();
    if (httpCode != 200) {
        _lastError = "HTTP " + String(httpCode);
        http.end();
        return false;
    }

    int totalSize = http.getSize();
    Serial.printf("OPDS download: %d bytes from %s\n", totalSize, url.c_str());

    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        _lastError = "No stream";
        http.end();
        return false;
    }

    File file = SD.open(targetPath, FILE_WRITE);
    if (!file) {
        _lastError = "Cannot open " + targetPath;
        http.end();
        return false;
    }

    uint8_t buf[1024];
    int downloaded = 0;
    int lastPct = -1;

    while (http.connected() && (totalSize < 0 || downloaded < totalSize)) {
        size_t avail = stream->available();
        if (avail == 0) {
            delay(10);
            continue;
        }

        size_t toRead = min(avail, sizeof(buf));
        int bytesRead = stream->readBytes(buf, toRead);
        if (bytesRead <= 0) break;

        file.write(buf, bytesRead);
        downloaded += bytesRead;

        if (totalSize > 0 && onProgress) {
            int pct = (downloaded * 100) / totalSize;
            if (pct != lastPct) {
                onProgress(pct);
                lastPct = pct;
            }
        }
    }

    file.close();
    http.end();

    Serial.printf("OPDS download: saved %d bytes to %s\n", downloaded, targetPath.c_str());

    if (totalSize > 0 && downloaded < totalSize) {
        _lastError = "Incomplete download";
        SD.remove(targetPath);
        return false;
    }

    if (onProgress) onProgress(100);
    return true;
}

// ─── Server config persistence ──────────────────────────────────────

static const char* SERVERS_PATH = "/books/.opds_servers.json";

std::vector<OpdsServer> opds_load_servers() {
    std::vector<OpdsServer> servers;

    File f = SD.open(SERVERS_PATH, FILE_READ);
    if (!f) {
        // Create defaults
        OpdsServer gutenberg;
        gutenberg.name = "Project Gutenberg";
        gutenberg.baseUrl = "https://www.gutenberg.org/ebooks/search.opds/?sort_order=downloads";
        gutenberg.isDefault = true;
        servers.push_back(gutenberg);

        OpdsServer calibre;
        calibre.name = "My Calibre";
        calibre.isDefault = false;
        servers.push_back(calibre);

        opds_save_servers(servers);
        return servers;
    }

    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("OPDS servers: parse error %s\n", err.c_str());
        return servers;
    }

    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        OpdsServer s;
        s.name      = obj["name"].as<String>();
        s.baseUrl   = obj["baseUrl"].as<String>();
        s.username  = obj["username"].as<String>();
        s.password  = obj["password"].as<String>();
        s.isDefault = obj["isDefault"] | false;
        servers.push_back(s);
    }

    Serial.printf("OPDS: loaded %d servers\n", servers.size());
    return servers;
}

void opds_save_servers(const std::vector<OpdsServer>& servers) {
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& s : servers) {
        JsonObject obj = arr.createNestedObject();
        obj["name"]      = s.name;
        obj["baseUrl"]   = s.baseUrl;
        obj["username"]  = s.username;
        obj["password"]  = s.password;
        obj["isDefault"] = s.isDefault;
    }

    File f = SD.open(SERVERS_PATH, FILE_WRITE);
    if (f) {
        serializeJson(doc, f);
        f.close();
        Serial.printf("OPDS: saved %d servers\n", servers.size());
    }
}
