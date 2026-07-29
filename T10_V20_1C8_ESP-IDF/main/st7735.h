// Minimal, self-contained ST7735 driver for the LilyGO TTGO T10 V2.0 (1.8" TFT).
// Pins and panel variant come from the repo's User_Setups/T10_V20_1C8.h.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ST7735_WIDTH  128
#define ST7735_HEIGHT 160

// 16-bit RGB565 colors (assuming the MADCTL color order below is correct).
#define ST_BLACK   0x0000
#define ST_WHITE   0xFFFF
#define ST_RED     0xF800
#define ST_GREEN   0x07E0
#define ST_BLUE    0x001F
#define ST_YELLOW  0xFFE0
#define ST_CYAN    0x07FF
#define ST_MAGENTA 0xF81F
#define ST_ORANGE  0xFC00
#define ST_GRAY    0x8410

// Pack r,g,b (0-255) into RGB565.
static inline uint16_t st_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void st7735_init(void);
void st7735_backlight(bool on);

void st7735_fill_screen(uint16_t color);
void st7735_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void st7735_draw_pixel(int16_t x, int16_t y, uint16_t color);

// Blit a w*h RGB565 image. `data` is big-endian byte pairs (hi,lo per pixel),
// typically a const array in flash (see tools/img2c.py / boot_image.h).
void st7735_draw_image(int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *data);

// Text using the built-in 5x7 font. `size` scales the glyph (1 = 6x8 cell).
void st7735_draw_char(int16_t x, int16_t y, char c, uint16_t color, uint16_t bg, uint8_t size);
void st7735_draw_string(int16_t x, int16_t y, const char *s, uint16_t color, uint16_t bg, uint8_t size);
