#include "st7735.h"
#include "font5x7.h"

#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "st7735";

// --- Pin map for the T10 V2.0 (from repo User_Setups/T10_V20_1C8.h) ---------
#define PIN_MOSI 23
#define PIN_SCLK 5
#define PIN_CS   16
#define PIN_DC   17
#define PIN_BL   27
// No hardware reset line on this board -> we use the software reset command.

// --- Panel tuning (GREENTAB2). Adjust these if the image is shifted / mirrored
//     / colors look swapped on first run. -------------------------------------
#define ST7735_COLSTART 2      // horizontal pixel offset
#define ST7735_ROWSTART 1      // vertical pixel offset
#define ST7735_MADCTL   0xC8   // MY|MX|BGR : portrait, origin top-left, BGR order
#define ST7735_SPI_HZ   (20 * 1000 * 1000)

#define ST7735_SPI_HOST SPI2_HOST

static spi_device_handle_t s_spi;

// Static, word-aligned (DMA-capable) scratch buffers.
static WORD_ALIGNED_ATTR uint8_t s_scratch[64];
static WORD_ALIGNED_ATTR uint8_t s_fillbuf[2048];   // 1024 pixels per chunk

// Low-level SPI write. dc=0 -> command, dc=1 -> data. Synchronous (polling).
static void spi_send(const uint8_t *data, int len, int dc)
{
    if (len <= 0) return;
    gpio_set_level(PIN_DC, dc);
    spi_transaction_t t = {0};
    t.length = (size_t)len * 8;
    if (len <= (int)sizeof(s_scratch)) {
        memcpy(s_scratch, data, len);
        t.tx_buffer = s_scratch;      // always DMA-safe
    } else {
        t.tx_buffer = data;           // caller must pass a DMA-capable buffer
    }
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static inline void wr_cmd(uint8_t c)            { spi_send(&c, 1, 0); }
static inline void wr_data(const uint8_t *d, int n) { spi_send(d, n, 1); }
static inline void wr_data1(uint8_t d)          { spi_send(&d, 1, 1); }

static void st7735_init_seq(void)
{
    wr_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150)); // SWRESET
    wr_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(150)); // SLPOUT

    wr_cmd(0xB1); { uint8_t d[] = {0x01,0x2C,0x2D};                 wr_data(d,3); } // FRMCTR1
    wr_cmd(0xB2); { uint8_t d[] = {0x01,0x2C,0x2D};                 wr_data(d,3); } // FRMCTR2
    wr_cmd(0xB3); { uint8_t d[] = {0x01,0x2C,0x2D,0x01,0x2C,0x2D};  wr_data(d,6); } // FRMCTR3
    wr_cmd(0xB4); wr_data1(0x07);                                                   // INVCTR

    wr_cmd(0xC0); { uint8_t d[] = {0xA2,0x02,0x84}; wr_data(d,3); } // PWCTR1
    wr_cmd(0xC1); wr_data1(0xC5);                                   // PWCTR2
    wr_cmd(0xC2); { uint8_t d[] = {0x0A,0x00};      wr_data(d,2); } // PWCTR3
    wr_cmd(0xC3); { uint8_t d[] = {0x8A,0x2A};      wr_data(d,2); } // PWCTR4
    wr_cmd(0xC4); { uint8_t d[] = {0x8A,0xEE};      wr_data(d,2); } // PWCTR5
    wr_cmd(0xC5); wr_data1(0x0E);                                   // VMCTR1

    wr_cmd(0x20);                                                   // INVOFF (green tab)
    wr_cmd(0x36); wr_data1(ST7735_MADCTL);                          // MADCTL
    wr_cmd(0x3A); wr_data1(0x05);                                   // COLMOD = 16bpp

    wr_cmd(0xE0); { uint8_t d[] = {0x02,0x1C,0x07,0x12,0x37,0x32,0x29,0x2D,
                                   0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10};
                    wr_data(d,16); }                                // GMCTRP1
    wr_cmd(0xE1); { uint8_t d[] = {0x03,0x1D,0x07,0x06,0x2E,0x2C,0x29,0x2D,
                                   0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10};
                    wr_data(d,16); }                                // GMCTRN1

    wr_cmd(0x13); vTaskDelay(pdMS_TO_TICKS(10));  // NORON
    wr_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(100)); // DISPON
}

static void set_window(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    x0 += ST7735_COLSTART; x1 += ST7735_COLSTART;
    y0 += ST7735_ROWSTART; y1 += ST7735_ROWSTART;
    wr_cmd(0x2A); { uint8_t d[] = {0x00,(uint8_t)x0,0x00,(uint8_t)x1}; wr_data(d,4); } // CASET
    wr_cmd(0x2B); { uint8_t d[] = {0x00,(uint8_t)y0,0x00,(uint8_t)y1}; wr_data(d,4); } // RASET
    wr_cmd(0x2C);                                                                       // RAMWR
}

void st7735_backlight(bool on)
{
    gpio_set_level(PIN_BL, on ? 1 : 0);
}

void st7735_init(void)
{
    // DC and backlight as outputs.
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(PIN_BL, 0);   // keep dark until the panel is initialised

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(s_fillbuf) + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(ST7735_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = ST7735_SPI_HZ,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(ST7735_SPI_HOST, &devcfg, &s_spi));

    st7735_init_seq();
    st7735_fill_screen(ST_BLACK);
    st7735_backlight(true);
    ESP_LOGI(TAG, "ST7735 ready (%dx%d)", ST7735_WIDTH, ST7735_HEIGHT);
}

void st7735_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= ST7735_WIDTH || y >= ST7735_HEIGHT) return;
    if (x + w > ST7735_WIDTH)  w = ST7735_WIDTH  - x;
    if (y + h > ST7735_HEIGHT) h = ST7735_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    set_window(x, y, x + w - 1, y + h - 1);

    uint8_t hi = color >> 8, lo = color & 0xFF;
    int total_px = (int)w * (int)h;
    int buf_px = sizeof(s_fillbuf) / 2;
    int prime = total_px < buf_px ? total_px : buf_px;
    for (int i = 0; i < prime; i++) {
        s_fillbuf[i * 2]     = hi;
        s_fillbuf[i * 2 + 1] = lo;
    }
    while (total_px > 0) {
        int chunk = total_px < buf_px ? total_px : buf_px;
        spi_send(s_fillbuf, chunk * 2, 1);
        total_px -= chunk;
    }
}

void st7735_fill_screen(uint16_t color)
{
    st7735_fill_rect(0, 0, ST7735_WIDTH, ST7735_HEIGHT, color);
}

void st7735_draw_pixel(int16_t x, int16_t y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= ST7735_WIDTH || y >= ST7735_HEIGHT) return;
    set_window(x, y, x, y);
    uint8_t d[2] = { color >> 8, color & 0xFF };
    wr_data(d, 2);
}

void st7735_draw_char(int16_t x, int16_t y, char c, uint16_t color, uint16_t bg, uint8_t size)
{
    if (size < 1) size = 1;
    if (c < FONT5X7_FIRST || c > FONT5X7_LAST) c = '?';
    const uint8_t *glyph = font5x7[c - FONT5X7_FIRST];

    for (int col = 0; col < 6; col++) {                 // 5 glyph cols + 1 spacer
        uint8_t bits = (col < 5) ? glyph[col] : 0x00;
        for (int row = 0; row < 8; row++) {
            uint16_t px = (bits & (1 << row)) ? color : bg;
            if (size == 1) {
                st7735_draw_pixel(x + col, y + row, px);
            } else {
                st7735_fill_rect(x + col * size, y + row * size, size, size, px);
            }
        }
    }
}

void st7735_draw_string(int16_t x, int16_t y, const char *s, uint16_t color, uint16_t bg, uint8_t size)
{
    if (size < 1) size = 1;
    int16_t cx = x;
    for (; *s; s++) {
        if (*s == '\n') { y += 8 * size; cx = x; continue; }
        st7735_draw_char(cx, y, *s, color, bg, size);
        cx += 6 * size;
    }
}
