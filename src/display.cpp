#include "display.h"
#include "config.h"
#include "epd_driver.h"
// Sans-serif fonts (FiraSans) — 5 sizes: XS(9pt) S(11pt) M(14pt) ML(17pt) L(20pt)
#include "font_xs.h"
#include "font_s.h"
#include "font_m.h"
#include "font_ml.h"
#include "font_l.h"
// Serif fonts (NotoSerif) — same 5 sizes
#include "font_serif_xs.h"
#include "font_serif_s.h"
#include "font_serif_m.h"
#include "font_serif_ml.h"
#include "font_serif_l.h"
#include "miniz.h"
// EPD whine mitigation: the epdiy I2S bus clock can produce an audible
// whine.  Aggressive approaches (clock-gating I2S1, driving CKH/CKV LOW)
// caused device lockups because those pins are shared with boot strapping.
// For now, we rely on epd_poweroff() which disables the high-voltage
// converters.  The residual I2S clock whine is a known hardware limitation
// of the T5 4.7" V2.4 board.

// Portrait framebuffer: 540 wide × 960 tall, 4bpp packed
static uint8_t* _pfb = nullptr;
// Landscape framebuffer for pushing to hardware: 960 wide × 540 tall
static uint8_t* _lfb = nullptr;

// 5 size levels × 2 families (sans index 0, serif index 1)
static const GFXfont* _sans_fonts[] = {
    &FiraSansXS, &FiraSansS, &FiraSansM, &FiraSansML, &FiraSansL
};
static const GFXfont* _serif_fonts[] = {
    &NotoSerifXS, &NotoSerifS, &NotoSerifM, &NotoSerifML, &NotoSerifL
};
static const int FONT_SIZE_COUNT = 5;
static const GFXfont* _font = &FiraSansM;  // default (level 2 = M)

// Full refresh counter — every N partial updates, do a full refresh
static int _partialCount = 0;
static const int PARTIAL_REFRESH_INTERVAL = 10;

// ─── Glyph Bitmap LRU Cache (PSRAM) ──────────────────────────────────────

static const int GLYPH_CACHE_SIZE = 16;  // number of cached glyphs
static const int GLYPH_CACHE_MAX_BMP = 512; // max bitmap bytes per entry

struct GlyphCacheEntry {
    const GFXfont* font;    // which font (acts as font_id)
    uint32_t codepoint;     // glyph codepoint
    uint8_t bitmap[GLYPH_CACHE_MAX_BMP]; // decompressed bitmap
    uint16_t bmpSize;       // actual bitmap size
    uint32_t lastUsed;      // access counter for LRU
    bool valid;
};

static GlyphCacheEntry _glyphCache[GLYPH_CACHE_SIZE];
static uint32_t _glyphCacheAccessCounter = 0;

static void glyph_cache_init() {
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        _glyphCache[i].valid = false;
        _glyphCache[i].lastUsed = 0;
    }
    _glyphCacheAccessCounter = 0;
}

// Find cached bitmap or return nullptr
static uint8_t* glyph_cache_find(const GFXfont* font, uint32_t cp) {
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (_glyphCache[i].valid && _glyphCache[i].font == font && _glyphCache[i].codepoint == cp) {
            _glyphCache[i].lastUsed = ++_glyphCacheAccessCounter;
            return _glyphCache[i].bitmap;
        }
    }
    return nullptr;
}

// Store decompressed bitmap in cache, evicting LRU entry if needed
static uint8_t* glyph_cache_store(const GFXfont* font, uint32_t cp, const uint8_t* bmp, uint16_t size) {
    if (size > GLYPH_CACHE_MAX_BMP) return nullptr; // too large to cache

    // Find empty slot or LRU slot
    int slot = -1;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (!_glyphCache[i].valid) {
            slot = i;
            break;
        }
        if (_glyphCache[i].lastUsed < oldest) {
            oldest = _glyphCache[i].lastUsed;
            slot = i;
        }
    }
    if (slot < 0) slot = 0; // safety

    _glyphCache[slot].font = font;
    _glyphCache[slot].codepoint = cp;
    memcpy(_glyphCache[slot].bitmap, bmp, size);
    _glyphCache[slot].bmpSize = size;
    _glyphCache[slot].lastUsed = ++_glyphCacheAccessCounter;
    _glyphCache[slot].valid = true;

    return _glyphCache[slot].bitmap;
}

// ─── Portrait framebuffer pixel ops ────────────────────────────────

static inline void pset(int x, int y, uint8_t g4) {
    if ((unsigned)x >= PORTRAIT_W || (unsigned)y >= PORTRAIT_H) return;
    int idx = y * (PORTRAIT_W / 2) + x / 2;
    if (x & 1) _pfb[idx] = (_pfb[idx] & 0xF0) | (g4 & 0x0F);
    else        _pfb[idx] = (_pfb[idx] & 0x0F) | ((g4 & 0x0F) << 4);
}

static inline uint8_t pget(int x, int y) {
    if ((unsigned)x >= PORTRAIT_W || (unsigned)y >= PORTRAIT_H) return 15;
    int idx = y * (PORTRAIT_W / 2) + x / 2;
    return (x & 1) ? (_pfb[idx] & 0x0F) : ((_pfb[idx] >> 4) & 0x0F);
}

// ─── UTF-8 decoder ─────────────────────────────────────────────────

static uint32_t next_codepoint(const uint8_t** pp) {
    const uint8_t* p = *pp;
    if (*p == 0) return 0;
    uint32_t cp;
    int bytes;
    if (*p < 0x80)        { cp = *p; bytes = 1; }
    else if (*p < 0xC0)   { cp = 0xFFFD; bytes = 1; }
    else if (*p < 0xE0)   { cp = *p & 0x1F; bytes = 2; }
    else if (*p < 0xF0)   { cp = *p & 0x0F; bytes = 3; }
    else                   { cp = *p & 0x07; bytes = 4; }
    for (int i = 1; i < bytes; i++) {
        if ((p[i] & 0xC0) != 0x80) { *pp = p + i; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *pp = p + bytes;
    return cp;
}

// ─── Custom glyph renderer (portrait buffer) ──────────────────────

static void draw_glyph(int* cursor_x, int cursor_y, uint32_t cp, uint8_t fg) {
    GFXglyph* glyph = nullptr;
    get_glyph(_font, cp, &glyph);
    if (!glyph) return;

    uint8_t w = glyph->width;
    uint8_t h = glyph->height;
    int32_t left = glyph->left;
    int32_t byte_w = (w / 2 + w % 2);
    unsigned long bmp_size = byte_w * h;

    uint8_t* bitmap = nullptr;
    bool needsFree = false;

    if (_font->compressed) {
        // Check glyph cache first
        bitmap = glyph_cache_find(_font, cp);
        if (!bitmap) {
            // Cache miss — decompress
            uint8_t* tmpBuf = (uint8_t*)malloc(bmp_size);
            if (!tmpBuf) { *cursor_x += glyph->advance_x; return; }
            mz_ulong dest_len = bmp_size;
            if (mz_uncompress(tmpBuf, &dest_len,
                              &_font->bitmap[glyph->data_offset],
                              glyph->compressed_size) != MZ_OK) {
                free(tmpBuf);
                *cursor_x += glyph->advance_x;
                return;
            }
            // Try to store in cache
            uint8_t* cached = glyph_cache_store(_font, cp, tmpBuf, (uint16_t)bmp_size);
            if (cached) {
                free(tmpBuf);
                bitmap = cached;
            } else {
                // Too large for cache, use tmp and free later
                bitmap = tmpBuf;
                needsFree = true;
            }
        }
    } else {
        bitmap = &_font->bitmap[glyph->data_offset];
    }

    // Build color lookup: glyph value 0=bg(white), 15=full fg
    uint8_t lut[16];
    for (int c = 0; c < 16; c++) {
        int val = 15 + c * ((int)fg - 15) / 15;
        lut[c] = (uint8_t)(val < 0 ? 0 : (val > 15 ? 15 : val));
    }

    for (int gy = 0; gy < h; gy++) {
        int py = cursor_y - glyph->top + gy;
        if (py < 0 || py >= PORTRAIT_H) continue;
        for (int gx = 0; gx < w; gx++) {
            int px = *cursor_x + left + gx;
            if (px < 0 || px >= PORTRAIT_W) continue;
            uint8_t bm = bitmap[gy * byte_w + gx / 2];
            uint8_t val = (gx & 1) ? (bm >> 4) : (bm & 0x0F);
            if (val > 0) pset(px, py, lut[val]);
        }
    }

    if (needsFree) free(bitmap);
    *cursor_x += glyph->advance_x;
}

// ─── Public API ────────────────────────────────────────────────────

void display_init() {
    epd_init();
    epd_poweroff();
    // whine mitigation removed — see comment at top of file

    _pfb = (uint8_t*)ps_calloc(PORTRAIT_W * PORTRAIT_H / 2, 1);
    _lfb = (uint8_t*)ps_calloc(PHYS_WIDTH * PHYS_HEIGHT / 2, 1);
    if (!_pfb || !_lfb) {
        Serial.println("ERROR: framebuffer alloc failed");
        return;
    }
    memset(_pfb, 0xFF, PORTRAIT_W * PORTRAIT_H / 2);
    memset(_lfb, 0xFF, PHYS_WIDTH * PHYS_HEIGHT / 2);

    glyph_cache_init();
}

void display_clear() {
    memset(_pfb, 0xFF, PORTRAIT_W * PORTRAIT_H / 2);

    epd_poweron();
    epd_clear();
    epd_poweroff_all();

    _partialCount = 0;
}

void display_fill_screen(uint8_t gray4) {
    uint8_t byte = (gray4 & 0x0F) | ((gray4 & 0x0F) << 4);
    memset(_pfb, byte, PORTRAIT_W * PORTRAIT_H / 2);
}

void display_draw_text(int x, int y, const char* text, uint8_t fg_color) {
    if (!_pfb || !text) return;
    const uint8_t* p = (const uint8_t*)text;
    int cx = x;
    uint32_t cp;
    while ((cp = next_codepoint(&p)) != 0) {
        draw_glyph(&cx, y, cp, fg_color);
    }
}

void display_draw_pixel(int x, int y, uint8_t gray4) {
    if (!_pfb) return;
    pset(x, y, gray4);
}

void display_draw_filled_rect(int x, int y, int w, int h, uint8_t gray4) {
    if (!_pfb) return;
    int x0 = max(0, x), y0 = max(0, y);
    int x1 = min(PORTRAIT_W, x + w), y1 = min(PORTRAIT_H, y + h);
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            pset(px, py, gray4);
        }
    }
}

void display_draw_hline(int x, int y, int w, uint8_t gray4) {
    if (!_pfb || y < 0 || y >= PORTRAIT_H) return;
    int x0 = max(0, x), x1 = min(PORTRAIT_W, x + w);
    for (int px = x0; px < x1; px++) pset(px, y, gray4);
}

void display_draw_vline(int x, int y, int h, uint8_t gray4) {
    if (!_pfb || x < 0 || x >= PORTRAIT_W) return;
    int y0 = max(0, y), y1 = min(PORTRAIT_H, y + h);
    for (int py = y0; py < y1; py++) pset(x, py, gray4);
}

void display_draw_rect(int x, int y, int w, int h, uint8_t gray4) {
    if (w <= 0 || h <= 0) return;
    display_draw_hline(x, y, w, gray4);
    display_draw_hline(x, y + h - 1, w, gray4);
    display_draw_vline(x, y, h, gray4);
    display_draw_vline(x + w - 1, y, h, gray4);
}

// ─── Optimized rotation ─────────────────────────────────────────────
// Process portrait framebuffer in column-major order.
// Each portrait column (fixed px, varying py) maps to a landscape row
// at ly = (PW-1) - px, with lx = py.
// Process two portrait pixels at a time where possible.

static void rotatePortraitToLandscape() {
    const int pw = PORTRAIT_W;
    const int ph = PORTRAIT_H;
    const int p_stride = pw / 2;       // bytes per portrait row
    const int l_stride = PHYS_WIDTH / 2; // bytes per landscape row

    memset(_lfb, 0xFF, PHYS_WIDTH * PHYS_HEIGHT / 2);

    for (int px = 0; px < pw; px++) {
        int ly = (pw - 1) - px;
        uint8_t* lrow = _lfb + ly * l_stride;

        // Portrait column px: read from _pfb[py * p_stride + px/2]
        int p_byte_col = px / 2;
        bool px_odd = px & 1;

        for (int py = 0; py < ph; py++) {
            uint8_t pbyte = _pfb[py * p_stride + p_byte_col];
            uint8_t val = px_odd ? (pbyte & 0x0F) : ((pbyte >> 4) & 0x0F);

            // lx = py — epdiy expects even pixels in LOW nibble, odd in HIGH
            int lx = py;
            int l_byte = lx / 2;
            if (lx & 1) {
                lrow[l_byte] = (lrow[l_byte] & 0x0F) | ((val & 0x0F) << 4);
            } else {
                lrow[l_byte] = (lrow[l_byte] & 0xF0) | (val & 0x0F);
            }
        }
    }
}

static Rect_t portraitRectToLandscape(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > PORTRAIT_W) w = PORTRAIT_W - x;
    if (y + h > PORTRAIT_H) h = PORTRAIT_H - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    Rect_t area;
    area.x = y;
    area.y = PORTRAIT_W - (x + w);
    area.width = h;
    area.height = w;
    return area;
}

static uint8_t* extractLandscapeArea(Rect_t area) {
    if (!_lfb || area.width <= 0 || area.height <= 0) return nullptr;

    int srcStride = PHYS_WIDTH / 2;
    int dstStride = (area.width + 1) / 2;
    size_t bytes = (size_t)dstStride * area.height;
    uint8_t* out = (uint8_t*)malloc(bytes);
    if (!out) return nullptr;
    memset(out, 0xFF, bytes);

    for (int row = 0; row < area.height; row++) {
        const uint8_t* srcRow = _lfb + (area.y + row) * srcStride;
        uint8_t* dstRow = out + row * dstStride;

        for (int col = 0; col < area.width; col++) {
            int srcX = area.x + col;
            uint8_t srcByte = srcRow[srcX / 2];
            uint8_t val = (srcX & 1) ? ((srcByte >> 4) & 0x0F) : (srcByte & 0x0F);

            int dstByte = col / 2;
            if (col & 1) dstRow[dstByte] = (dstRow[dstByte] & 0x0F) | ((val & 0x0F) << 4);
            else         dstRow[dstByte] = (dstRow[dstByte] & 0xF0) | (val & 0x0F);
        }
    }

    return out;
}

// Full refresh: clear display then draw
void display_update() {
    if (!_pfb || !_lfb) return;

    unsigned long t0 = millis();
    rotatePortraitToLandscape();
    unsigned long t1 = millis();
    Serial.printf("Rotation: %lums\n", t1 - t0);


    epd_poweron();
    epd_clear_area_cycles(epd_full_screen(), 6, 50);
    epd_draw_grayscale_image(epd_full_screen(), _lfb);
    epd_poweroff_all();  // zero all control bits — fully silences TPS65185 oscillator

    _partialCount = 0;
}

// Sleep refresh: preserve the panel's normal post-refresh hold state so the
// sleep image remains latched correctly while the MCU enters deep sleep.
void display_update_sleep() {
    if (!_pfb || !_lfb) return;

    unsigned long t0 = millis();
    rotatePortraitToLandscape();
    unsigned long t1 = millis();
    Serial.printf("Sleep rotation: %lums\n", t1 - t0);

    epd_poweron();
    epd_clear_area_cycles(epd_full_screen(), 6, 50);
    epd_draw_grayscale_image(epd_full_screen(), _lfb);
    epd_poweroff_all();  // zero all control bits for clean power state

    _partialCount = 0;
}

// Medium refresh: 2 EPD clear cycles.
void display_update_medium() {
    if (!_pfb || !_lfb) return;

    unsigned long t0 = millis();
    rotatePortraitToLandscape();
    unsigned long t1 = millis();
    Serial.printf("Rotation: %lums\n", t1 - t0);


    epd_poweron();
    epd_clear_area_cycles(epd_full_screen(), 2, 50);
    epd_draw_grayscale_image(epd_full_screen(), _lfb);
    epd_poweroff_all();  // zero all control bits — fully silences TPS65185 oscillator

    _partialCount = 0;
}

// Fast refresh: light 1-cycle clear. Used for page turns.
void display_update_fast() {
    if (!_pfb || !_lfb) return;

    unsigned long t0 = millis();
    rotatePortraitToLandscape();
    unsigned long t1 = millis();
    Serial.printf("Rotation: %lums\n", t1 - t0);


    epd_poweron();
    epd_clear_area_cycles(epd_full_screen(), 1, 40);
    epd_draw_grayscale_image(epd_full_screen(), _lfb);
    epd_poweroff_all();  // zero all control bits — fully silences TPS65185 oscillator

    _partialCount = 0;
}

// Rotate only a sub-region of the portrait framebuffer to landscape.
// Returns a freshly allocated buffer sized to the landscape sub-region.
// The caller must free() the returned buffer.
static uint8_t* rotatePortraitRegion(int px, int py, int pw, int ph, Rect_t& outArea) {
    // Clamp to portrait bounds
    if (px < 0) { pw += px; px = 0; }
    if (py < 0) { ph += py; py = 0; }
    if (px + pw > PORTRAIT_W) pw = PORTRAIT_W - px;
    if (py + ph > PORTRAIT_H) ph = PORTRAIT_H - py;
    if (pw <= 0 || ph <= 0) return nullptr;

    // Landscape coords: lx = py..py+ph-1, ly = (PW-1)-(px+pw-1)..(PW-1)-px
    outArea.x = py;
    outArea.y = (PORTRAIT_W - 1) - (px + pw - 1);
    outArea.width = ph;
    outArea.height = pw;

    int l_stride = (outArea.width + 1) / 2;  // bytes per row in output
    size_t outBytes = (size_t)l_stride * outArea.height;
    uint8_t* out = (uint8_t*)malloc(outBytes);
    if (!out) return nullptr;
    memset(out, 0xFF, outBytes);

    int p_stride = PORTRAIT_W / 2;  // bytes per portrait row

    for (int col = px; col < px + pw; col++) {
        // Portrait column 'col' maps to landscape row ly = (PW-1) - col
        int ly = (PORTRAIT_W - 1) - col;
        int outRow = ly - outArea.y;  // row index within output buffer
        uint8_t* dstRow = out + outRow * l_stride;

        int p_byte_col = col / 2;
        bool col_odd = col & 1;

        for (int row = py; row < py + ph; row++) {
            // Read portrait pixel
            uint8_t pbyte = _pfb[row * p_stride + p_byte_col];
            uint8_t val = col_odd ? (pbyte & 0x0F) : ((pbyte >> 4) & 0x0F);

            // Landscape lx = row, offset within output = lx - outArea.x
            int outCol = row - outArea.x;
            int dstByte = outCol / 2;
            if (outCol & 1) {
                dstRow[dstByte] = (dstRow[dstByte] & 0x0F) | ((val & 0x0F) << 4);
            } else {
                dstRow[dstByte] = (dstRow[dstByte] & 0xF0) | (val & 0x0F);
            }
        }
    }

    return out;
}

void display_update_reader_body(int x, int y, int w, int h, bool strongCleanup) {
    if (!_pfb) return;

    unsigned long t0 = millis();
    Rect_t area;
    uint8_t* region = rotatePortraitRegion(x, y, w, h, area);
    unsigned long t1 = millis();
    Serial.printf("Partial rotation: %lums area=%ldx%ld (was full=%dx%d)\n",
                  t1 - t0, (long)area.width, (long)area.height, PORTRAIT_W, PORTRAIT_H);

    if (!region || area.width <= 0 || area.height <= 0) {
        if (region) free(region);
        display_update_fast();
        return;
    }

    epd_poweron();
    epd_clear_area_cycles(area, strongCleanup ? 2 : 1, strongCleanup ? 40 : 30);
    epd_draw_grayscale_image(area, region);
    epd_poweroff_all();

    free(region);
    _partialCount = 0;
}

// Partial update: draw without clearing (no white flash)
void display_update_partial() {
    if (!_pfb || !_lfb) return;

    rotatePortraitToLandscape();


    epd_poweron();
    epd_draw_grayscale_image(epd_full_screen(), _lfb);
    epd_poweroff_all();

    _partialCount++;
}

// Smart update: use partial unless full refresh is needed
void display_update_mode(bool fullRefresh) {
    if (fullRefresh || _partialCount >= PARTIAL_REFRESH_INTERVAL) {
        display_update();
    } else {
        display_update_partial();
    }
}

int display_text_width(const char* text) {
    if (!text || !*text) return 0;
    int w = 0;
    const uint8_t* p = (const uint8_t*)text;
    uint32_t cp;
    while ((cp = next_codepoint(&p)) != 0) {
        GFXglyph* glyph = nullptr;
        get_glyph(_font, cp, &glyph);
        if (glyph) w += glyph->advance_x;
    }
    return w;
}

int display_font_height() {
    return _font->advance_y;
}

int display_font_ascender() {
    return _font->ascender;
}

int display_width() {
    return PORTRAIT_W;
}

int display_height() {
    return PORTRAIT_H;
}

void display_set_font_size(int sizeLevel) {
    // Legacy: maps old 0/1/2 to new levels, always sans
    // New callers pass 0-6 directly which also works fine
    display_set_font(sizeLevel, false);
}

void display_set_font(int sizeLevel, bool serif) {
    if (sizeLevel < 0) sizeLevel = 0;
    if (sizeLevel >= FONT_SIZE_COUNT) sizeLevel = FONT_SIZE_COUNT - 1;
    const GFXfont* newFont = serif ? _serif_fonts[sizeLevel] : _sans_fonts[sizeLevel];
    if (newFont != _font) {
        _font = newFont;
        glyph_cache_init();  // invalidate cache on font change
    }
    Serial.printf("Font set to %s level %d (advance_y=%d)\n",
                  serif ? "serif" : "sans", sizeLevel, _font->advance_y);
}

void display_power_off() {
    epd_poweroff_all();  // zero all control bits for full TPS65185 standby
}
