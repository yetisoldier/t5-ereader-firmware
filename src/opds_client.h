#pragma once

#include <Arduino.h>
#include <vector>
#include <functional>

struct OpdsEntry {
    String title;
    String author;
    String acquisitionUrl;   // Direct EPUB download URL
    String detailUrl;        // OPDS detail feed URL (for nested resolve)
    String mimeType;         // "application/epub+zip"
    bool   isNavigation;     // true = subcatalog link, false = book
    String navUrl;           // URL if navigation entry
};

struct OpdsServer {
    String name;
    String baseUrl;
    String username;
    String password;
    bool   isDefault;
};

class OpdsClient {
public:
    std::vector<OpdsEntry> fetchCatalog(const String& url,
                                         const String& username = "",
                                         const String& password = "");
    bool downloadEpub(const String& url,
                      const String& targetPath,
                      const String& username = "",
                      const String& password = "",
                      std::function<void(int pct)> onProgress = nullptr);
    String resolveEpubUrl(const String& detailUrl,
                          const String& username = "",
                          const String& password = "",
                          int maxDepth = 4);
    String getNextPageUrl() const { return _nextPageUrl; }
    String getLastError() const { return _lastError; }

private:
    String _nextPageUrl;
    String _lastError;
    static constexpr const char* EPUB_MIME = "application/epub+zip";

    String resolveUrl(const String& base, const String& relative);
};

// Server config persistence
std::vector<OpdsServer> opds_load_servers();
void opds_save_servers(const std::vector<OpdsServer>& servers);
