#pragma once

#include <Arduino.h>
#include <vector>

// Lightweight ZIP reader using ESP-IDF built-in zlib
struct ZipEntry {
    String name;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_header_offset;
    uint16_t compression_method;  // 0=STORED, 8=DEFLATE
};

class ZipReader {
public:
    bool open(const char* path);
    void close();
    uint8_t* readFile(const char* name, size_t* out_size);
    bool fileExists(const char* name);

private:
    FILE* _f = nullptr;
    std::vector<ZipEntry> _entries;
    bool parseCentralDirectory();
};

// EPUB parser
struct SpineItem {
    String id;
    String href;  // full path within ZIP
};

struct ManifestItem {
    String id;
    String href;        // full path within ZIP
    String mediaType;
    String properties;
};

struct TocEntry {
    String label;
    String href;
    int chapterIndex = -1;
};

class EpubParser {
public:
    bool open(const char* filepath);
    void close();

    const String& getTitle() const { return _title; }
    int getChapterCount() const { return _spine.size(); }
    String getChapterText(int index);
    String getChapterHtml(int index);
    String getChapterTitle(int index);
    int getTocCount() const { return _toc.size(); }
    String getTocLabel(int index) const;
    int getTocChapterIndex(int index) const;
    const std::vector<SpineItem>& getSpine() const { return _spine; }
    const std::vector<ManifestItem>& getManifest() const { return _manifest; }
    String getCoverImagePath() const { return _coverImagePath; }
    bool hasCoverImage() const { return _coverImagePath.length() > 0; }
    uint8_t* readAsset(const String& zipPath, size_t* outSize);
    String resolveChapterAssetPath(int chapterIndex, const String& relativePath);

    // Chapter title cache access for progress JSON persistence
    const std::vector<String>& getChapterTitleCache() const { return _chapterTitleCache; }
    void setChapterTitleCache(const std::vector<String>& titles);

private:
    ZipReader _zip;
    String _title;
    String _basePath;
    std::vector<SpineItem> _spine;
    std::vector<ManifestItem> _manifest;
    std::vector<TocEntry> _toc;
    String _coverImagePath;
    std::vector<String> _chapterTitleCache;

    bool parseContainer();
    bool parseContentOpf(const char* opfPath);
    bool parseNavigationDocument(const String& navPath);
    bool parseNcx(const String& ncxPath);
    void buildSpineFallbackToc();
    int findSpineIndexForHref(const String& href) const;
    String stripHtml(const char* html, size_t len);
    String resolveRelativePath(const String& base, const String& relative) const;
};
