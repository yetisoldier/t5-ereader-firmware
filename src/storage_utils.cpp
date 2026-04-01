#include "storage_utils.h"

#include <cstdio>

static String vfs_path(const String& path) {
    if (path.startsWith("/sd")) return path;
    return String("/sd") + path;
}

bool storage_write_text_atomic(const String& finalPath, const String& tmpPath, const String& contents) {
    String tmpVfs = vfs_path(tmpPath);
    String finalVfs = vfs_path(finalPath);

    FILE* f = fopen(tmpVfs.c_str(), "wb");
    if (!f) {
        return false;
    }

    size_t len = contents.length();
    size_t written = fwrite(contents.c_str(), 1, len, f);
    fclose(f);
    if (written != len) {
        remove(tmpVfs.c_str());
        return false;
    }

    remove(finalVfs.c_str());
    if (rename(tmpVfs.c_str(), finalVfs.c_str()) == 0) {
        return true;
    }

    // Fallback for filesystems that reject rename semantics unexpectedly.
    FILE* src = fopen(tmpVfs.c_str(), "rb");
    if (!src) {
        remove(tmpVfs.c_str());
        return false;
    }
    FILE* dst = fopen(finalVfs.c_str(), "wb");
    if (!dst) {
        fclose(src);
        remove(tmpVfs.c_str());
        return false;
    }

    char buf[512];
    bool ok = true;
    while (!feof(src)) {
        size_t n = fread(buf, 1, sizeof(buf), src);
        if (n > 0 && fwrite(buf, 1, n, dst) != n) {
            ok = false;
            break;
        }
        if (ferror(src)) {
            ok = false;
            break;
        }
    }

    fclose(src);
    fclose(dst);
    remove(tmpVfs.c_str());
    return ok;
}
