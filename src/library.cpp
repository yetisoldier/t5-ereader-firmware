#include "library.h"
#include "config.h"
#include "epub.h"
#include "storage_utils.h"
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

static bool _mounted = false;
static const char* CACHE_PATH = "/books/.library_cache.json";
static const char* CACHE_TMP_PATH = "/books/.library_cache.tmp";

static bool ensure_dir(const char* path, const char* label) {
    if (SD.exists(path)) {
        Serial.printf("Storage: %s ready: %s\n", label, path);
        return true;
    }

    Serial.printf("Storage: %s missing, creating: %s\n", label, path);
    if (SD.mkdir(path)) {
        Serial.printf("Storage: %s created: %s\n", label, path);
        return true;
    }

    Serial.printf("Storage: failed to create %s: %s\n", label, path);
    return false;
}

bool library_init() {
    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, SPI)) {
        Serial.println("SD card mount failed");
        return false;
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD card: %llu MB\n", cardSize);

    bool storageReady = true;
    storageReady &= ensure_dir(BOOKS_DIR, "books library");
    storageReady &= ensure_dir(PROGRESS_DIR, "reading progress cache");
    storageReady &= ensure_dir(LINE_CACHE_DIR, "line cache");
    storageReady &= ensure_dir(SLEEP_IMAGES_DIR, "sleep images");

    if (!storageReady) {
        Serial.println("Storage: one or more app folders are unavailable; continuing with existing fallbacks");
    }

    _mounted = true;
    return true;
}

// ─── Metadata cache ─────────────────────────────────────────────────

struct CacheEntry {
    String path;
    size_t size;
    String title;
    int chapters;
    bool hasCover;
    String coverPath;
};

static std::vector<CacheEntry> loadCache() {
    std::vector<CacheEntry> cache;
    File f = SD.open(CACHE_PATH, FILE_READ);
    if (!f) return cache;

    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return cache;

    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        CacheEntry e;
        e.path     = obj["path"].as<String>();
        e.size     = obj["size"] | 0;
        e.title    = obj["title"].as<String>();
        e.chapters = obj["chapters"] | 0;
        e.hasCover = obj["hasCover"] | false;
        e.coverPath = obj["coverPath"].as<String>();
        cache.push_back(e);
    }
    Serial.printf("Cache: loaded %d entries\n", cache.size());
    return cache;
}

static void saveCache(const std::vector<CacheEntry>& cache) {
    DynamicJsonDocument doc(8192);
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& e : cache) {
        JsonObject obj = arr.createNestedObject();
        obj["path"]     = e.path;
        obj["size"]     = e.size;
        obj["title"]    = e.title;
        obj["chapters"] = e.chapters;
        obj["hasCover"] = e.hasCover;
        obj["coverPath"] = e.coverPath;
    }
    String json;
    serializeJson(doc, json);
    if (storage_write_text_atomic(CACHE_PATH, CACHE_TMP_PATH, json)) {
        Serial.printf("Cache: saved %d entries\n", cache.size());
    } else {
        Serial.println("Cache: atomic save failed");
    }
}

static const CacheEntry* findCacheEntry(const std::vector<CacheEntry>& cache,
                                         const String& path, size_t fileSize) {
    for (const auto& e : cache) {
        if (e.path == path && e.size == fileSize) return &e;
    }
    return nullptr;
}

// ─── Directory scanning ─────────────────────────────────────────────

static void scanDir(File& dir, const char* path,
                    std::vector<BookInfo>& books,
                    const std::vector<CacheEntry>& cache,
                    std::vector<CacheEntry>& newCache) {
    File entry;
    while ((entry = dir.openNextFile()) && books.size() < MAX_BOOKS) {
        if (entry.isDirectory()) {
            String subpath = String(path) + "/" + entry.name();
            scanDir(entry, subpath.c_str(), books, cache, newCache);
        } else {
            String name = String(entry.name());
            name.toLowerCase();
            if (name.endsWith(".epub")) {
                BookInfo book;
                book.filepath = String(path) + "/" + entry.name();
                book.fileSize = entry.size();

                // Check cache first
                const CacheEntry* cached = findCacheEntry(cache, book.filepath, book.fileSize);
                if (cached) {
                    book.title = cached->title;
                    book.totalChapters = cached->chapters;
                    book.hasCover = cached->hasCover;
                    book.coverPath = cached->coverPath;
                    Serial.printf("Cache hit: %s\n", book.title.c_str());
                } else {
                    // Cache miss — open EPUB to extract metadata
                    EpubParser parser;
                    if (parser.open(book.filepath.c_str())) {
                        book.title = parser.getTitle();
                        book.totalChapters = parser.getChapterCount();
                        book.hasCover = parser.hasCoverImage();
                        book.coverPath = parser.getCoverImagePath();
                        parser.close();
                    } else {
                        book.title = entry.name();
                        int dot = book.title.lastIndexOf('.');
                        if (dot > 0) book.title = book.title.substring(0, dot);
                    }
                    Serial.printf("Cache miss: %s -> %s\n", book.title.c_str(), book.filepath.c_str());
                }

                // Add to new cache
                CacheEntry ce;
                ce.path = book.filepath;
                ce.size = book.fileSize;
                ce.title = book.title;
                ce.chapters = book.totalChapters;
                ce.hasCover = book.hasCover;
                ce.coverPath = book.coverPath;
                newCache.push_back(ce);

                books.push_back(book);
            }
        }
        entry.close();
    }
}

std::vector<BookInfo> library_scan() {
    std::vector<BookInfo> books;
    if (!_mounted) return books;

    unsigned long startMs = millis();

    // Load existing cache
    std::vector<CacheEntry> cache = loadCache();
    std::vector<CacheEntry> newCache;

    // Scan /books/ directory
    File booksDir = SD.open(BOOKS_DIR);
    if (booksDir) {
        scanDir(booksDir, BOOKS_DIR, books, cache, newCache);
        booksDir.close();
    }

    // Also scan root for epub files
    File root = SD.open("/");
    if (root) {
        File entry;
        while ((entry = root.openNextFile()) && books.size() < MAX_BOOKS) {
            if (!entry.isDirectory()) {
                String name = String(entry.name());
                name.toLowerCase();
                if (name.endsWith(".epub")) {
                    String filepath = String("/") + entry.name();
                    bool dupe = false;
                    for (const auto& b : books) {
                        if (b.filepath == filepath) { dupe = true; break; }
                    }
                    if (!dupe) {
                        BookInfo book;
                        book.filepath = filepath;
                        book.fileSize = entry.size();

                        const CacheEntry* cached = findCacheEntry(cache, book.filepath, book.fileSize);
                        if (cached) {
                            book.title = cached->title;
                            book.totalChapters = cached->chapters;
                            book.hasCover = cached->hasCover;
                            book.coverPath = cached->coverPath;
                        } else {
                            EpubParser parser;
                            if (parser.open(book.filepath.c_str())) {
                                book.title = parser.getTitle();
                                book.totalChapters = parser.getChapterCount();
                                book.hasCover = parser.hasCoverImage();
                                book.coverPath = parser.getCoverImagePath();
                                parser.close();
                            } else {
                                book.title = entry.name();
                                int dot = book.title.lastIndexOf('.');
                                if (dot > 0) book.title = book.title.substring(0, dot);
                            }
                        }

                        CacheEntry ce;
                        ce.path = book.filepath;
                        ce.size = book.fileSize;
                        ce.title = book.title;
                        ce.chapters = book.totalChapters;
                        ce.hasCover = book.hasCover;
                        ce.coverPath = book.coverPath;
                        newCache.push_back(ce);

                        books.push_back(book);
                    }
                }
            }
            entry.close();
        }
        root.close();
    }

    // Save updated cache (stale entries automatically pruned since we only
    // add entries for files that still exist on disk)
    saveCache(newCache);

    // Load saved reading progress for each book
    for (auto& book : books) {
        String name = book.filepath;
        int ls = name.lastIndexOf('/');
        if (ls >= 0) name = name.substring(ls + 1);
        String progPath = String(PROGRESS_DIR) + "/" + name + ".json";

        File pf = SD.open(progPath, FILE_READ);
        if (pf) {
            StaticJsonDocument<512> doc;
            if (deserializeJson(doc, pf) == DeserializationError::Ok) {
                book.progressChapter = doc["chapter"] | 0;
                book.progressPage = doc["page"] | 0;
                book.totalChapters = doc["total_chapters"] | book.totalChapters;
                book.hasProgress = true;
                book.lastReadOrder = doc["last_read"] | (uint32_t)0;
            }
            pf.close();
        }
    }

    unsigned long elapsed = millis() - startMs;
    Serial.printf("Library: %d books found in %lums\n", books.size(), elapsed);
    return books;
}

int library_find_current_book(const std::vector<BookInfo>& books) {
    // Find the book with the highest lastReadOrder (most recently read)
    int bestIdx = -1;
    uint32_t bestOrder = 0;

    for (int i = 0; i < (int)books.size(); i++) {
        if (books[i].hasProgress) {
            if (books[i].lastReadOrder > bestOrder || bestIdx < 0) {
                bestOrder = books[i].lastReadOrder;
                bestIdx = i;
            }
        }
    }
    return bestIdx;
}
