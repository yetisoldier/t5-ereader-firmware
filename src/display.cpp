#include "display.h"
#include "config.h"
#include "epd_driver.h"
// Sans-serif fonts (FiraSans) — 7 sizes: XS(9pt) S(11pt) M(14pt) ML(17pt) L(20pt) XL(24pt) XXL(28pt)
#include "font_xs.h"
#include "font_s.h"
#include "font_m.h"
#include "font_ml.h"
#include "font_l.h"
#include "font_xl.h"
#include "font_xxl.h"
// Serif fonts (NotoSerif) — same 7 sizes
#include "font_serif_xs.h"
#include "font_serif_s.h"
#include "font_serif_m.h"
#include "font_serif_ml.h"
#include "font_serif_l.h"
#include "font_serif_xl.h"
#include "font_serif_xxl.h"
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

// 7 size levels × 2 families (sans index 0, serif index 1)
static const GFXfont* _sans_fonts[] = {
    &FiraSansXS, &FiraSansS, &FiraSansM, &FiraSansML,
    &FiraSansL, &FiraSansXL, &FiraSansXXL
};
static const GFXfont* _serif_fonts[] = {
    &NotoSerifXS, &NotoSerifS, &NotoSerifM, &NotoSerifML,
    &NotoSerifL, &NotoSerifXL, &NotoSerifXXL
};
static const int FONT_SIZE_COUNT = 7;
static const GFXfont* _font = &FiraSansM;  // default (level 2 = M)

// Full refresh counter — every N partial updates, do a full refresh
static int _partialCount = 0;
static const int PARTIAL_REFRESH_INTERVAL = 10;

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
    if (_font->compressed) {
        bitmap = (uint8_t*)malloc(bmp_size);
        if (!bitmap) { *cursor_x += glyph->advance_x; return; }
        mz_ulong dest_len = bmp_size;
        if (mz_uncompress(bitmap, &dest_len,
                          &_font->bitmap[glyph->data_offset],
                          glyph->compressed_size) != MZ_OK) {
            free(bitmap);
            *cursor_x += glyph->advance_x;
            return;
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

    if (_font->compressed) free(bitmap);
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
    epd_poweroff();

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

void display_update_reader_body(int x, int y, int w, int h, bool strongCleanup) {
    if (!_pfb || !_lfb) return;

    unsigned long t0 = millis();
    rotatePortraitToLandscape();
    Rect_t area = portraitRectToLandscape(x, y, w, h);
    uint8_t* region = extractLandscapeArea(area);
    unsigned long t1 = millis();
    Serial.printf("Rotation+extract: %lums area=%ldx%ld\n", t1 - t0, (long)area.width, (long)area.height);

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
    _font = serif ? _serif_fonts[sizeLevel] : _sans_fonts[sizeLevel];
    Serial.printf("Font set to %s level %d (advance_y=%d)\n",
                  serif ? "serif" : "sans", sizeLevel, _font->advance_y);
}

void display_power_off() {
    epd_poweroff_all();  // zero all control bits for full TPS65185 standby
}
