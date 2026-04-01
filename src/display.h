#pragma once

#include <Arduino.h>

void display_init();
void display_clear();
void display_fill_screen(uint8_t gray4);
void display_draw_text(int x, int y, const char* text, uint8_t fg_color = 0);
void display_draw_pixel(int x, int y, uint8_t gray4);
void display_draw_filled_rect(int x, int y, int w, int h, uint8_t gray4);
void display_draw_hline(int x, int y, int w, uint8_t gray4);
void display_draw_vline(int x, int y, int h, uint8_t gray4);
void display_draw_rect(int x, int y, int w, int h, uint8_t gray4);
void display_update();               // full refresh (heavy clear + draw, ~3s, 6 cycles)
void display_update_medium();        // medium refresh for chapter jumps (~1s, 2 cycles)
void display_update_fast();          // lighter full-screen refresh for page turns (1 cycle)
void display_update_reader_body(int x, int y, int w, int h, bool strongCleanup = false);
void display_update_partial();        // partial update (no clear, no flash)
void display_update_mode(bool fullRefresh);  // select mode
int  display_text_width(const char* text);
int  display_font_height();
int  display_font_ascender();
int  display_width();
int  display_height();
void display_set_font_size(int size);  // 0=small(14pt), 1=medium(20pt), 2=large(28pt)
void display_power_off();             // ensure EPD power rail is off
