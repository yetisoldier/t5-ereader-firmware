#include "epub.h"
#include "miniz.h"
#define z_stream     mz_stream
#define Z_OK         MZ_OK
#define Z_STREAM_END MZ_STREAM_END
#define Z_FINISH     MZ_FINISH
#define MAX_WBITS    15
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════════
// ZipReader — lightweight ZIP reader using ESP-IDF zlib
// ═══════════════════════════════════════════════════════════════════

static uint16_t read16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t read32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }
static String decodeEntities(const String& input);

bool ZipReader::open(const char* path) {
    close();

    // On ESP32 Arduino, SD card is mounted at /sd in VFS.
    // Arduino SD.open() handles this transparently, but POSIX fopen() needs
    // the full VFS path.  Prepend /sd if the caller passes a bare SD path.
    String vfsPath;
    if (strncmp(path, "/sd", 3) == 0) {
        vfsPath = path;
    } else {
        vfsPath = String("/sd") + path;
    }

    Serial.printf("ZIP: opening %s (vfs: %s)\n", path, vfsPath.c_str());
    _f = fopen(vfsPath.c_str(), "rb");
    if (!_f) {
        Serial.printf("ZIP: cannot open %s (errno %d)\n", vfsPath.c_str(), errno);
        return false;
    }
    return parseCentralDirectory();
}

void ZipReader::close() {
    if (_f) { fclose(_f); _f = nullptr; }
    _entries.clear();
}

bool ZipReader::parseCentralDirectory() {
    // Find End of Central Directory record (search backwards)
    fseek(_f, 0, SEEK_END);
    long fileSize = ftell(_f);
    long searchStart = (fileSize > 65558) ? fileSize - 65558 : 0;
    long searchLen = fileSize - searchStart;

    uint8_t* buf = (uint8_t*)malloc(searchLen);
    if (!buf) return false;

    fseek(_f, searchStart, SEEK_SET);
    fread(buf, 1, searchLen, _f);

    long eocdOff = -1;
    for (long i = searchLen - 22; i >= 0; i--) {
        if (buf[i] == 0x50 && buf[i+1] == 0x4b &&
            buf[i+2] == 0x05 && buf[i+3] == 0x06) {
            eocdOff = i;
            break;
        }
    }
    if (eocdOff < 0) { free(buf); return false; }

    uint16_t numEntries = read16(buf + eocdOff + 10);
    uint32_t cdSize     = read32(buf + eocdOff + 12);
    uint32_t cdOffset   = read32(buf + eocdOff + 16);
    free(buf);

    // Read central directory
    uint8_t* cd = (uint8_t*)malloc(cdSize);
    if (!cd) return false;

    fseek(_f, cdOffset, SEEK_SET);
    fread(cd, 1, cdSize, _f);

    uint32_t pos = 0;
    for (int i = 0; i < numEntries && pos + 46 <= cdSize; i++) {
        if (read32(cd + pos) != 0x02014b50) break;

        ZipEntry entry;
        entry.compression_method = read16(cd + pos + 10);
        entry.compressed_size    = read32(cd + pos + 20);
        entry.uncompressed_size  = read32(cd + pos + 24);
        entry.local_header_offset = read32(cd + pos + 42);

        uint16_t nameLen    = read16(cd + pos + 28);
        uint16_t extraLen   = read16(cd + pos + 30);
        uint16_t commentLen = read16(cd + pos + 32);

        if (pos + 46 + nameLen > cdSize) break;

        char* name = (char*)malloc(nameLen + 1);
        memcpy(name, cd + pos + 46, nameLen);
        name[nameLen] = 0;
        entry.name = String(name);
        free(name);

        _entries.push_back(entry);
        pos += 46 + nameLen + extraLen + commentLen;
    }
    free(cd);

    Serial.printf("ZIP: found %d entries\n", _entries.size());
    return _entries.size() > 0;
}

bool ZipReader::fileExists(const char* name) {
    for (const auto& e : _entries) {
        if (e.name == name) return true;
    }
    return false;
}

uint8_t* ZipReader::readFile(const char* name, size_t* outSize) {
    *outSize = 0;

    for (const auto& entry : _entries) {
        if (entry.name != name) continue;

        // Read local file header to find data offset
        fseek(_f, entry.local_header_offset, SEEK_SET);
        uint8_t lfh[30];
        if (fread(lfh, 1, 30, _f) != 30) return nullptr;
        if (read32(lfh) != 0x04034b50) return nullptr;

        uint16_t nameLen  = read16(lfh + 26);
        uint16_t extraLen = read16(lfh + 28);
        long dataOffset = entry.local_header_offset + 30 + nameLen + extraLen;
        fseek(_f, dataOffset, SEEK_SET);

        if (entry.compression_method == 0) {
            // STORED — just read directly
            uint8_t* data = (uint8_t*)ps_malloc(entry.uncompressed_size + 1);
            if (!data) return nullptr;
            fread(data, 1, entry.uncompressed_size, _f);
            data[entry.uncompressed_size] = 0;
            *outSize = entry.uncompressed_size;
            return data;

        } else if (entry.compression_method == 8) {
            // DEFLATE — inflate using zlib
            uint8_t* compressed = (uint8_t*)malloc(entry.compressed_size);
            if (!compressed) return nullptr;
            fread(compressed, 1, entry.compressed_size, _f);

            uint8_t* data = (uint8_t*)ps_malloc(entry.uncompressed_size + 1);
            if (!data) { free(compressed); return nullptr; }

            z_stream strm;
            memset(&strm, 0, sizeof(strm));
            strm.next_in   = compressed;
            strm.avail_in  = entry.compressed_size;
            strm.next_out  = data;
            strm.avail_out = entry.uncompressed_size;

            if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
                free(compressed); free(data); return nullptr;
            }
            int ret = inflate(&strm, Z_FINISH);
            inflateEnd(&strm);
            free(compressed);

            if (ret != Z_STREAM_END && ret != Z_OK) {
                free(data);
                return nullptr;
            }

            data[entry.uncompressed_size] = 0;
            *outSize = entry.uncompressed_size;
            return data;
        }
        break;
    }
    return nullptr;
}


// ═══════════════════════════════════════════════════════════════════
// Simple XML helpers (no dependency on tinyxml2)
// ═══════════════════════════════════════════════════════════════════

// Find attribute value: <...tag... attr="value"...>
static String xmlAttr(const char* xml, const char* tag, const char* attr) {
    const char* p = xml;
    while ((p = strstr(p, tag)) != nullptr) {
        const char* tagEnd = strchr(p, '>');
        if (!tagEnd) { p++; continue; }

        String attrSearch = String(attr) + "=\"";
        const char* a = strstr(p, attrSearch.c_str());
        if (a && a < tagEnd) {
            a += attrSearch.length();
            const char* end = strchr(a, '"');
            if (end && end < tagEnd + 1) {
                return String(a, end - a);
            }
        }
        p = tagEnd;
    }
    return "";
}

// Find text between <tag>...</tag>
static String xmlText(const char* xml, const char* tag) {
    String openTag = String("<") + tag;
    const char* p = strstr(xml, openTag.c_str());
    if (!p) return "";
    const char* start = strchr(p, '>');
    if (!start) return "";
    start++;
    String closeTag = String("</") + tag + ">";
    const char* end = strstr(start, closeTag.c_str());
    if (!end) return "";
    return String(start, end - start);
}


// ═══════════════════════════════════════════════════════════════════
// EpubParser
// ═══════════════════════════════════════════════════════════════════

bool EpubParser::open(const char* filepath) {
    close();
    if (!_zip.open(filepath)) return false;
    if (!parseContainer()) { close(); return false; }
    return true;
}

void EpubParser::close() {
    _zip.close();
    _title = "";
    _basePath = "";
    _spine.clear();
    _manifest.clear();
    _toc.clear();
    _coverImagePath = "";
    _chapterTitleCache.clear();
}

bool EpubParser::parseContainer() {
    size_t size;
    uint8_t* data = _zip.readFile("META-INF/container.xml", &size);
    if (!data) {
        Serial.println("EPUB: cannot read container.xml");
        return false;
    }

    String opfPath = xmlAttr((const char*)data, "rootfile", "full-path");
    free(data);

    if (opfPath.length() == 0) {
        Serial.println("EPUB: no rootfile in container.xml");
        return false;
    }

    Serial.printf("EPUB: content.opf at %s\n", opfPath.c_str());
    return parseContentOpf(opfPath.c_str());
}

bool EpubParser::parseContentOpf(const char* opfPath) {
    size_t size;
    uint8_t* data = _zip.readFile(opfPath, &size);
    if (!data) {
        Serial.printf("EPUB: cannot read %s\n", opfPath);
        return false;
    }

    // Extract base path (directory of OPF file)
    _basePath = String(opfPath);
    int lastSlash = _basePath.lastIndexOf('/');
    if (lastSlash >= 0) {
        _basePath = _basePath.substring(0, lastSlash + 1);
    } else {
        _basePath = "";
    }

    const char* xml = (const char*)data;

    // Extract title — try both dc:title and title
    _title = xmlText(xml, "dc:title");
    if (_title.length() == 0) {
        _title = xmlText(xml, "title");
    }
    if (_title.length() == 0) {
        _title = "Untitled";
    }

    Serial.printf("EPUB: title = %s\n", _title.c_str());

    // Build manifest and detect cover image metadata.
    _manifest.clear();
    _toc.clear();
    _coverImagePath = "";
    String coverId;
    String navDocPath;
    String ncxPath;

    const char* p = xml;
    while ((p = strstr(p, "<meta")) != nullptr) {
        const char* metaEnd = strchr(p, '>');
        if (!metaEnd) break;

        const char* namePos = strstr(p, "name=\"");
        const char* contentPos = strstr(p, "content=\"");
        if (namePos && contentPos && namePos < metaEnd && contentPos < metaEnd) {
            namePos += 6;
            const char* nameEnd = strchr(namePos, '"');
            contentPos += 9;
            const char* contentEnd = strchr(contentPos, '"');
            if (nameEnd && contentEnd && nameEnd <= metaEnd && contentEnd <= metaEnd) {
                String name = String(namePos, nameEnd - namePos);
                if (name == "cover") {
                    coverId = String(contentPos, contentEnd - contentPos);
                }
            }
        }
        p = metaEnd + 1;
    }

    p = xml;
    while ((p = strstr(p, "<item ")) != nullptr) {
        const char* itemEnd = strchr(p, '>');
        if (!itemEnd) break;

        // Extract id/href/media metadata from this <item> tag
        String id, href, mediaType, properties;
        const char* scan = p;

        // Extract id
        const char* idPos = strstr(scan, "id=\"");
        if (idPos && idPos < itemEnd) {
            idPos += 4;
            const char* idEnd = strchr(idPos, '"');
            if (idEnd) id = String(idPos, idEnd - idPos);
        }

        // Extract href
        const char* hrefPos = strstr(scan, "href=\"");
        if (hrefPos && hrefPos < itemEnd) {
            hrefPos += 6;
            const char* hrefEnd = strchr(hrefPos, '"');
            if (hrefEnd) href = String(hrefPos, hrefEnd - hrefPos);
        }

        const char* mediaPos = strstr(scan, "media-type=\"");
        if (mediaPos && mediaPos < itemEnd) {
            mediaPos += 12;
            const char* mediaEnd = strchr(mediaPos, '"');
            if (mediaEnd) mediaType = String(mediaPos, mediaEnd - mediaPos);
        }

        const char* propsPos = strstr(scan, "properties=\"");
        if (propsPos && propsPos < itemEnd) {
            propsPos += 12;
            const char* propsEnd = strchr(propsPos, '"');
            if (propsEnd) properties = String(propsPos, propsEnd - propsPos);
        }

        if (id.length() > 0 && href.length() > 0) {
            ManifestItem item;
            item.id = id;
            item.href = _basePath + href;
            item.mediaType = mediaType;
            item.properties = properties;
            _manifest.push_back(item);

            String hrefLower = href;
            hrefLower.toLowerCase();
            bool isImage = mediaType == "image/jpeg" || mediaType == "image/jpg" || mediaType == "image/png";
            String propsLower = properties;
            propsLower.toLowerCase();
            if (_coverImagePath.length() == 0) {
                if ((properties.indexOf("cover-image") >= 0 && isImage) ||
                    (coverId.length() > 0 && id == coverId) ||
                    (isImage && hrefLower.indexOf("cover") >= 0)) {
                    _coverImagePath = item.href;
                }
            }

            if (navDocPath.length() == 0 && propsLower.indexOf("nav") >= 0) {
                navDocPath = item.href;
            }
            if (ncxPath.length() == 0 && (mediaType == "application/x-dtbncx+xml" || hrefLower.endsWith(".ncx"))) {
                ncxPath = item.href;
            }
        }

        p = itemEnd + 1;
    }

    // Parse spine — <itemref idref="..."/>
    p = strstr(xml, "<spine");
    if (!p) { free(data); return false; }

    while ((p = strstr(p, "<itemref")) != nullptr) {
        String idref = xmlAttr(p, "itemref", "idref");
        if (idref.length() > 0) {
            // Find in manifest
            for (const auto& mi : _manifest) {
                if (mi.id == idref) {
                    SpineItem si;
                    si.id = mi.id;
                    si.href = mi.href;
                    _spine.push_back(si);
                    break;
                }
            }
        }
        p++;
    }

    if (_coverImagePath.length() == 0) {
        const char* guide = strstr(xml, "<guide");
        if (guide) {
            const char* gp = guide;
            while ((gp = strstr(gp, "<reference")) != nullptr) {
                const char* refEnd = strchr(gp, '>');
                if (!refEnd) break;
                String type = xmlAttr(gp, "reference", "type");
                String href = xmlAttr(gp, "reference", "href");
                type.toLowerCase();
                if (type.indexOf("cover") >= 0 && href.length() > 0) {
                    _coverImagePath = resolveRelativePath(_basePath, href);
                    break;
                }
                gp = refEnd + 1;
            }
        }
    }

    free(data);
    Serial.printf("EPUB: %d chapters in spine\n", _spine.size());
    _chapterTitleCache.assign(_spine.size(), String());

    // Try semantic TOC first (EPUB3 nav document, then EPUB2 NCX).
    // Fall back to spine-derived TOC only if both fail.
    // WDT-safe: stripHtml/decodeEntities now yield() periodically.
    bool haveToc = false;
    if (navDocPath.length() > 0) {
        haveToc = parseNavigationDocument(navDocPath);
        if (haveToc) {
            Serial.printf("EPUB: parsed nav document TOC (%d entries)\n", (int)_toc.size());
        }
    }
    if (!haveToc && ncxPath.length() > 0) {
        haveToc = parseNcx(ncxPath);
        if (haveToc) {
            Serial.printf("EPUB: parsed NCX TOC (%d entries)\n", (int)_toc.size());
        }
    }
    if (!haveToc) {
        buildSpineFallbackToc();
        Serial.printf("EPUB: using spine fallback TOC (%d entries)\n", (int)_toc.size());
    }

    // Manifest is only needed during OPF parsing to resolve spine hrefs.
    // Free it now to reclaim heap (72 items × 4 Strings each ≈ 15-20KB).
    _manifest.clear();
    _manifest.shrink_to_fit();

    Serial.printf("EPUB: open complete (heap: %d)\n", (int)ESP.getFreeHeap());
    return _spine.size() > 0;
}

static String stripFragment(const String& href) {
    int hash = href.indexOf('#');
    if (hash >= 0) return href.substring(0, hash);
    return href;
}

static String stripTagsAndTrim(const String& html) {
    String out;
    bool inTag = false;
    for (int i = 0; i < (int)html.length(); i++) {
        char c = html[i];
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag) out += c;
    }
    out.trim();
    return decodeEntities(out);
}

int EpubParser::findSpineIndexForHref(const String& href) const {
    String target = stripFragment(resolveRelativePath(_basePath, href));
    for (int i = 0; i < (int)_spine.size(); i++) {
        if (stripFragment(_spine[i].href) == target) return i;
    }
    return -1;
}

void EpubParser::buildSpineFallbackToc() {
    _toc.clear();
    for (int i = 0; i < (int)_spine.size(); i++) {
        TocEntry e;
        e.chapterIndex = i;
        e.href = _spine[i].href;
        // Use cached title if already available; otherwise use a cheap
        // placeholder.  Titles are loaded lazily via getChapterTitle()
        // when the TOC screen is actually displayed, avoiding 72 ZIP
        // reads during book open that consumed ~60 KB of heap.
        if (i < (int)_chapterTitleCache.size() && _chapterTitleCache[i].length() > 0) {
            e.label = _chapterTitleCache[i];
        } else {
            e.label = String("Section ") + String(i + 1);
        }
        _toc.push_back(e);
    }
}

bool EpubParser::parseNavigationDocument(const String& navPath) {
    size_t size = 0;
    uint8_t* data = _zip.readFile(navPath.c_str(), &size);
    if (!data) return false;

    String html = String((const char*)data);
    free(data);

    String lower = html;
    lower.toLowerCase();
    int navStart = lower.indexOf("epub:type=\"toc\"");
    if (navStart < 0) navStart = lower.indexOf("type=\"toc\"");
    if (navStart < 0) navStart = lower.indexOf("<nav");
    if (navStart < 0) return false;

    int navTagStart = lower.lastIndexOf("<nav", navStart);
    if (navTagStart >= 0) navStart = navTagStart;
    int navEnd = lower.indexOf("</nav>", navStart);
    if (navEnd < 0) return false;

    String navHtml = html.substring(navStart, navEnd);
    int pos = 0;
    int added = 0;
    while (true) {
        int aStart = navHtml.indexOf("<a", pos);
        if (aStart < 0) break;
        int tagEnd = navHtml.indexOf('>', aStart);
        if (tagEnd < 0) break;
        int close = navHtml.indexOf("</a>", tagEnd);
        if (close < 0) break;

        String tag = navHtml.substring(aStart, tagEnd + 1);
        String href = xmlAttr(tag.c_str(), "a", "href");
        String label = stripTagsAndTrim(navHtml.substring(tagEnd + 1, close));
        int chapterIndex = findSpineIndexForHref(resolveRelativePath(navPath.substring(0, navPath.lastIndexOf('/') + 1), href));
        if (href.length() > 0 && label.length() > 0 && chapterIndex >= 0) {
            TocEntry e;
            e.label = label;
            e.href = href;
            e.chapterIndex = chapterIndex;
            bool dup = false;
            for (const auto& existing : _toc) {
                if (existing.chapterIndex == e.chapterIndex && existing.label == e.label) { dup = true; break; }
            }
            if (!dup) {
                _toc.push_back(e);
                added++;
            }
        }
        pos = close + 4;
    }

    return added > 0;
}

bool EpubParser::parseNcx(const String& ncxPath) {
    size_t size = 0;
    uint8_t* data = _zip.readFile(ncxPath.c_str(), &size);
    if (!data) return false;

    String xml = String((const char*)data);
    free(data);

    int pos = 0;
    int added = 0;
    while (true) {
        yield();  // Prevent WDT on books with many navPoints
        int npStart = xml.indexOf("<navPoint", pos);
        if (npStart < 0) break;
        int npEnd = xml.indexOf("</navPoint>", npStart);
        if (npEnd < 0) break;
        String block = xml.substring(npStart, npEnd);
        String label = xmlText(block.c_str(), "text");
        String href = xmlAttr(block.c_str(), "content", "src");
        label.trim();
        int chapterIndex = findSpineIndexForHref(resolveRelativePath(ncxPath.substring(0, ncxPath.lastIndexOf('/') + 1), href));
        if (href.length() > 0 && label.length() > 0 && chapterIndex >= 0) {
            TocEntry e;
            e.label = label;
            e.href = href;
            e.chapterIndex = chapterIndex;
            bool dup = false;
            for (const auto& existing : _toc) {
                if (existing.chapterIndex == e.chapterIndex && existing.label == e.label) { dup = true; break; }
            }
            if (!dup) {
                _toc.push_back(e);
                added++;
            }
        }
        pos = npEnd + 11;
    }

    return added > 0;
}

String EpubParser::getTocLabel(int index) const {
    if (index < 0 || index >= (int)_toc.size()) return "";
    return _toc[index].label;
}

int EpubParser::getTocChapterIndex(int index) const {
    if (index < 0 || index >= (int)_toc.size()) return -1;
    return _toc[index].chapterIndex;
}

String EpubParser::resolveRelativePath(const String& base, const String& relative) const {
    if (relative.startsWith("/")) return relative;
    String result = base + relative;
    // Resolve ".." components
    while (true) {
        int dotdot = result.indexOf("/..");
        if (dotdot < 0) break;
        int prevSlash = result.lastIndexOf('/', dotdot - 1);
        if (prevSlash < 0) break;
        result = result.substring(0, prevSlash) + result.substring(dotdot + 3);
    }
    return result;
}

String EpubParser::getChapterText(int index) {
    if (index < 0 || index >= (int)_spine.size()) return "";

    Serial.printf("EPUB: reading chapter %d: %s (heap: %d)\n",
                  index, _spine[index].href.c_str(), (int)ESP.getFreeHeap());
    yield();

    size_t size;
    uint8_t* data = _zip.readFile(_spine[index].href.c_str(), &size);
    if (!data) {
        Serial.printf("EPUB: cannot read chapter %s\n", _spine[index].href.c_str());
        return "";
    }

    Serial.printf("EPUB: decompressed %d bytes, stripping HTML (heap: %d)\n",
                  (int)size, (int)ESP.getFreeHeap());
    yield();

    String text = stripHtml((const char*)data, size);
    free(data);

    Serial.printf("EPUB: stripped to %d chars (heap: %d)\n",
                  (int)text.length(), (int)ESP.getFreeHeap());

    // Resolve relative image paths in \x01IMG|path\x01 markers to absolute ZIP paths
    if (text.indexOf('\x01') >= 0) {
        String resolved;
        resolved.reserve(text.length() + 128);
        int pos = 0;
        int tlen = text.length();
        while (pos < tlen) {
            if (text[pos] == '\x01' && pos + 5 < tlen && text.substring(pos + 1, pos + 5) == "IMG|") {
                int end = text.indexOf('\x01', pos + 5);
                if (end > pos) {
                    String relPath = text.substring(pos + 5, end);
                    String absPath = resolveChapterAssetPath(index, relPath);
                    resolved += '\x01';
                    resolved += "IMG|";
                    resolved += absPath;
                    resolved += '\x01';
                    pos = end + 1;
                    continue;
                }
            }
            resolved += text[pos];
            pos++;
        }
        return resolved;
    }
    return text;
}

String EpubParser::getChapterHtml(int index) {
    if (index < 0 || index >= (int)_spine.size()) return "";

    size_t size;
    uint8_t* data = _zip.readFile(_spine[index].href.c_str(), &size);
    if (!data) return "";

    String html = String((const char*)data);
    free(data);
    return html;
}

uint8_t* EpubParser::readAsset(const String& zipPath, size_t* outSize) {
    return _zip.readFile(zipPath.c_str(), outSize);
}

String EpubParser::resolveChapterAssetPath(int chapterIndex, const String& relativePath) {
    if (chapterIndex < 0 || chapterIndex >= (int)_spine.size()) return relativePath;
    String chapterPath = _spine[chapterIndex].href;
    int lastSlash = chapterPath.lastIndexOf('/');
    String chapterBase = (lastSlash >= 0) ? chapterPath.substring(0, lastSlash + 1) : "";
    return resolveRelativePath(chapterBase, relativePath);
}

String EpubParser::getChapterTitle(int index) {
    if (index < 0 || index >= (int)_spine.size()) return "";

    if (index < (int)_chapterTitleCache.size() && _chapterTitleCache[index].length() > 0) {
        return _chapterTitleCache[index];
    }

    size_t size;
    uint8_t* data = _zip.readFile(_spine[index].href.c_str(), &size);
    if (!data) {
        String fallback = String("Chapter ") + String(index + 1);
        if (index < (int)_chapterTitleCache.size()) _chapterTitleCache[index] = fallback;
        return fallback;
    }

    const char* html = (const char*)data;

    // Try <title> tag first
    String title = xmlText(html, "title");
    if (title.length() > 0) {
        title.trim();
        if (title.length() > 0 && title != "Untitled") {
            free(data);
            if (index < (int)_chapterTitleCache.size()) _chapterTitleCache[index] = title;
            return title;
        }
    }

    // Try <h1>, <h2>, <h3>
    const char* hTags[] = {"h1", "h2", "h3"};
    for (const char* tag : hTags) {
        title = xmlText(html, tag);
        if (title.length() > 0) {
            // Strip any inner HTML tags from the heading
            String clean;
            bool inTag = false;
            for (int i = 0; i < (int)title.length(); i++) {
                if (title[i] == '<') inTag = true;
                else if (title[i] == '>') inTag = false;
                else if (!inTag) clean += title[i];
            }
            clean.trim();
            if (clean.length() > 0) {
                free(data);
                if (index < (int)_chapterTitleCache.size()) _chapterTitleCache[index] = clean;
                return clean;
            }
        }
    }

    free(data);

    // Fall back to spine ID
    String fallback = _spine[index].id.length() > 0 ? _spine[index].id : String("Chapter ") + String(index + 1);
    if (index < (int)_chapterTitleCache.size()) _chapterTitleCache[index] = fallback;
    return fallback;
}

void EpubParser::setChapterTitleCache(const std::vector<String>& titles) {
    // Inject persisted chapter titles into the in-memory cache, preventing ZIP
    // reads for titles that were already extracted on a previous open.
    int n = min((int)titles.size(), (int)_chapterTitleCache.size());
    for (int i = 0; i < n; i++) {
        if (titles[i].length() > 0) {
            _chapterTitleCache[i] = titles[i];
        }
    }
}

// Check if a tag name is a block-level element that should produce a line break
static bool isBlockTag(const String& tag) {
    return tag == "p" || tag == "/p" ||
           tag == "div" || tag == "/div" ||
           tag == "br" || tag == "br/" ||
           tag == "li" || tag == "/li" ||
           tag == "tr" || tag == "/tr" ||
           tag == "blockquote" || tag == "/blockquote" ||
           tag == "section" || tag == "/section" ||
           tag == "article" || tag == "/article" ||
           tag == "aside" || tag == "/aside" ||
           tag == "header" || tag == "/header" ||
           tag == "footer" || tag == "/footer" ||
           tag == "figcaption" || tag == "/figcaption" ||
           tag == "ol" || tag == "/ol" ||
           tag == "ul" || tag == "/ul" ||
           tag == "dl" || tag == "/dl" ||
           tag == "dt" || tag == "/dt" ||
           tag == "dd" || tag == "/dd" ||
           tag == "pre" || tag == "/pre" ||
           tag.startsWith("h") || tag.startsWith("/h");
}

// Decode a single numeric HTML entity (&#NNN; or &#xHH;) to UTF-8
static String decodeNumericEntity(const String& entity) {
    uint32_t codepoint = 0;
    if (entity.startsWith("&#x") || entity.startsWith("&#X")) {
        codepoint = strtoul(entity.c_str() + 3, nullptr, 16);
    } else if (entity.startsWith("&#")) {
        codepoint = strtoul(entity.c_str() + 2, nullptr, 10);
    }

    if (codepoint == 0) return "?";

    // Common typographic characters → ASCII equivalents for e-paper readability
    switch (codepoint) {
        case 160:   return " ";    // &nbsp;
        case 173:   return "";     // soft hyphen
        case 8194:  // en space
        case 8195:  // em space
        case 8201:  return " ";    // thin space
        case 8211:  return " - ";  // en dash
        case 8212:  return " -- "; // em dash
        case 8216:  return "'";    // left single quote
        case 8217:  return "'";    // right single quote
        case 8218:  return ",";    // single low-9 quote
        case 8220:  return "\"";   // left double quote
        case 8221:  return "\"";   // right double quote
        case 8222:  return "\"";   // double low-9 quote
        case 8226:  return "* ";   // bullet
        case 8230:  return "...";  // ellipsis
        case 8242:  return "'";    // prime
        case 8243:  return "\"";   // double prime
        case 8249:  return "<";    // single left angle quote
        case 8250:  return ">";    // single right angle quote
        case 8260:  return "/";    // fraction slash
        case 8364:  return "EUR "; // euro sign
    }

    // Encode to UTF-8 for characters the font may support
    char buf[5];
    if (codepoint < 0x80) {
        buf[0] = (char)codepoint; buf[1] = 0;
    } else if (codepoint < 0x800) {
        buf[0] = 0xC0 | (codepoint >> 6);
        buf[1] = 0x80 | (codepoint & 0x3F);
        buf[2] = 0;
    } else if (codepoint < 0x10000) {
        buf[0] = 0xE0 | (codepoint >> 12);
        buf[1] = 0x80 | ((codepoint >> 6) & 0x3F);
        buf[2] = 0x80 | (codepoint & 0x3F);
        buf[3] = 0;
    } else {
        buf[0] = 0xF0 | (codepoint >> 18);
        buf[1] = 0x80 | ((codepoint >> 12) & 0x3F);
        buf[2] = 0x80 | ((codepoint >> 6) & 0x3F);
        buf[3] = 0x80 | (codepoint & 0x3F);
        buf[4] = 0;
    }
    return String(buf);
}

// Decode all HTML entities in a string (named + numeric)
static String decodeEntities(const String& input) {
    String result;
    result.reserve(input.length());

    int i = 0;
    int len = input.length();
    unsigned long lastYieldMs = millis();
    while (i < len) {
        // Yield every 50ms to prevent watchdog timeout on large chapters
        unsigned long nowMs = millis();
        if (nowMs - lastYieldMs >= 50) {
            yield();
            lastYieldMs = nowMs;
        }

        if (input[i] == '&') {
            int semi = input.indexOf(';', i + 1);
            if (semi > i && semi - i < 12) {
                String entity = input.substring(i, semi + 1);
                String lower = entity;
                lower.toLowerCase();

                // Named entities
                if (lower == "&amp;")    { result += '&';  i = semi + 1; continue; }
                if (lower == "&lt;")     { result += '<';  i = semi + 1; continue; }
                if (lower == "&gt;")     { result += '>';  i = semi + 1; continue; }
                if (lower == "&quot;")   { result += '"';  i = semi + 1; continue; }
                if (lower == "&apos;")   { result += '\''; i = semi + 1; continue; }
                if (lower == "&nbsp;")   { result += ' ';  i = semi + 1; continue; }
                if (lower == "&mdash;")  { result += " -- "; i = semi + 1; continue; }
                if (lower == "&ndash;")  { result += " - ";  i = semi + 1; continue; }
                if (lower == "&lsquo;")  { result += '\''; i = semi + 1; continue; }
                if (lower == "&rsquo;")  { result += '\''; i = semi + 1; continue; }
                if (lower == "&ldquo;")  { result += '"';  i = semi + 1; continue; }
                if (lower == "&rdquo;")  { result += '"';  i = semi + 1; continue; }
                if (lower == "&hellip;") { result += "..."; i = semi + 1; continue; }
                if (lower == "&bull;")   { result += "* ";  i = semi + 1; continue; }
                if (lower == "&trade;")  { result += "(TM)"; i = semi + 1; continue; }
                if (lower == "&copy;")   { result += "(c)";  i = semi + 1; continue; }
                if (lower == "&reg;")    { result += "(R)";  i = semi + 1; continue; }
                if (lower == "&deg;")    { result += "deg";  i = semi + 1; continue; }
                if (lower == "&shy;")    { result += "";     i = semi + 1; continue; }

                // Numeric entities
                if (entity.startsWith("&#")) {
                    result += decodeNumericEntity(entity);
                    i = semi + 1;
                    continue;
                }

                // Unknown named entity — pass through as-is
            }
        }
        result += input[i];
        i++;
    }
    return result;
}

String EpubParser::stripHtml(const char* html, size_t len) {
    String result;
    result.reserve(len / 2);

    bool inTag = false;
    bool inScript = false;
    bool inStyle = false;
    bool collectingTag = false;
    bool isImgTag = false;     // only buffer tag content for img/image tags
    String tagName;
    String tagContent;
    unsigned long lastYieldMs = millis();

    for (size_t i = 0; i < len && html[i]; i++) {
        // Yield every 50ms to prevent watchdog timeout on large chapters
        if ((i & 0x3FF) == 0) {
            unsigned long nowMs = millis();
            if (nowMs - lastYieldMs >= 50) {
                yield();
                lastYieldMs = nowMs;
            }
        }

        char c = html[i];

        if (c == '<') {
            inTag = true;
            tagName = "";
            tagContent = "";
            collectingTag = true;
            isImgTag = false;
            continue;
        }

        if (c == '>') {
            inTag = false;
            collectingTag = false;
            tagName.toLowerCase();

            // Detect <img> and <image> tags — extract src and emit marker
            if (tagName == "img" || tagName == "image") {
                // Extract src="..." from buffered tag content
                // Scan raw HTML from tag start to find src= or href= attribute
                String src;
                const char* attrs = tagContent.c_str();
                const char* srcAttr = strstr(attrs, "src=");
                if (!srcAttr) srcAttr = strstr(attrs, "href=");
                if (srcAttr) {
                    srcAttr += (srcAttr[0] == 's') ? 4 : 5; // skip "src=" or "href="
                    if (*srcAttr == '"' || *srcAttr == '\'') {
                        char q = *srcAttr++;
                        const char* end = strchr(srcAttr, q);
                        if (end && end > srcAttr) {
                            src = String(srcAttr, end - srcAttr);
                        }
                    }
                }
                if (src.length() > 0) {
                    if (result.length() > 0 && result[result.length()-1] != '\n')
                        result += '\n';
                    result += '\x01';
                    result += "IMG|";
                    result += src;
                    result += '\x01';
                    result += '\n';
                }
            }

            // Skip script/style content
            if (tagName == "script") inScript = true;
            if (tagName == "/script") inScript = false;
            if (tagName == "style") inStyle = true;
            if (tagName == "/style") inStyle = false;

            // Block elements → newline
            if (isBlockTag(tagName)) {
                if (result.length() > 0 && result[result.length()-1] != '\n') {
                    result += '\n';
                }
            }
            continue;
        }

        if (inTag) {
            if (collectingTag && c != ' ' && c != '/' && c != '\n' && c != '\r') {
                tagName += c;
            } else {
                if (collectingTag) {
                    // Tag name complete — check if it's an img tag
                    collectingTag = false;
                    String lowerTag = tagName;
                    lowerTag.toLowerCase();
                    isImgTag = (lowerTag == "img" || lowerTag == "image");
                }
                // Only buffer tag content for img/image tags to save memory
                if (isImgTag) {
                    tagContent += c;
                }
            }
            continue;
        }

        if (inScript || inStyle) continue;

        // Collapse whitespace
        if (c == '\r') continue;
        if (c == '\n' || c == '\t') c = ' ';
        if (c == ' ' && result.length() > 0 && result[result.length()-1] == ' ') continue;

        result += c;
    }

    // Decode all HTML entities (named + numeric, including UTF-8)
    result = decodeEntities(result);

    // Trim leading/trailing whitespace
    result.trim();
    return result;
}
