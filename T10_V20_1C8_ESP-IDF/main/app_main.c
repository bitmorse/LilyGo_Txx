// LilyGO TTGO T10 V2.0 (1.8" ST7735) - ESP-IDF test firmware.
//
//   * splash + graphics demo on the TFT
//   * 3 onboard buttons drive a tiny menu
//       BTN1 (GPIO35) -> WiFi scan, results on screen + serial
//       BTN2 (GPIO34) -> color / graphics demo
//       BTN3 (GPIO39) -> board info (chip, heap, MAC)
//   * uptime/heap heartbeat over serial (`make monitor`)

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "st7735.h"
#include "buttons.h"
#include "wifi_scan.h"
#include "power.h"
#include "sound.h"
#include "boot_image.h"

static const char *TAG = "app";

#define HEADER_H 14
#define MAX_APS  8

// ---- small drawing helpers -------------------------------------------------

static void header(const char *title)
{
    st7735_fill_rect(0, 0, ST7735_WIDTH, HEADER_H, ST_BLUE);
    st7735_draw_string(3, 3, title, ST_WHITE, ST_BLUE, 1);
}

static void body_clear(void)
{
    st7735_fill_rect(0, HEADER_H, ST7735_WIDTH, ST7735_HEIGHT - HEADER_H, ST_BLACK);
}

static void screen_home(void)
{
    header("T10 V2.0  1.8in");
    body_clear();
    st7735_draw_string(4, 24, "ESP-IDF test",  ST_GREEN,  ST_BLACK, 1);
    st7735_draw_string(4, 34, "firmware",      ST_GREEN,  ST_BLACK, 1);

    st7735_draw_string(4, 58,  "BTN1: WiFi scan", ST_WHITE,  ST_BLACK, 1);
    st7735_draw_string(4, 72,  "BTN2: Colors",    ST_YELLOW, ST_BLACK, 1);
    st7735_draw_string(4, 86,  "BTN3: Board info", ST_CYAN,  ST_BLACK, 1);

    st7735_draw_string(4, 120, "Press a button", ST_GRAY, ST_BLACK, 1);
    st7735_draw_string(4, 130, "to start...",    ST_GRAY, ST_BLACK, 1);
}

// ---- BTN2: color / graphics demo ------------------------------------------

static void demo_colors(void)
{
    const uint16_t colors[] = { ST_RED, ST_GREEN, ST_BLUE, ST_YELLOW,
                                ST_CYAN, ST_MAGENTA, ST_WHITE };
    const char *names[]     = { "RED","GREEN","BLUE","YELLOW",
                                "CYAN","MAGENTA","WHITE" };
    for (int i = 0; i < 7; i++) {
        st7735_fill_screen(colors[i]);
        uint16_t txt = (colors[i] == ST_WHITE || colors[i] == ST_YELLOW ||
                        colors[i] == ST_CYAN) ? ST_BLACK : ST_WHITE;
        st7735_draw_string(30, 74, names[i], txt, colors[i], 2);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Concentric rectangles.
    st7735_fill_screen(ST_BLACK);
    const uint16_t ring[] = { ST_RED, ST_ORANGE, ST_YELLOW, ST_GREEN, ST_CYAN, ST_BLUE };
    for (int i = 0; i < 6; i++) {
        int inset = i * 10;
        st7735_fill_rect(inset, inset,
                         ST7735_WIDTH - 2 * inset, ST7735_HEIGHT - 2 * inset, ring[i]);
    }
    vTaskDelay(pdMS_TO_TICKS(1200));
}

// ---- BTN3: board info ------------------------------------------------------

static void screen_board_info(void)
{
    header("Board info");
    body_clear();

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char line[32];
    int y = 22;

    snprintf(line, sizeof(line), "ESP32 rev%d", chip.revision);
    st7735_draw_string(4, y, line, ST_WHITE, ST_BLACK, 1); y += 14;

    snprintf(line, sizeof(line), "%d cores", chip.cores);
    st7735_draw_string(4, y, line, ST_WHITE, ST_BLACK, 1); y += 14;

    snprintf(line, sizeof(line), "heap %uK",
             (unsigned)(esp_get_free_heap_size() / 1024));
    st7735_draw_string(4, y, line, ST_GREEN, ST_BLACK, 1); y += 14;

    st7735_draw_string(4, y, "MAC:", ST_GRAY, ST_BLACK, 1); y += 12;
    snprintf(line, sizeof(line), "%02X:%02X:%02X", mac[0], mac[1], mac[2]);
    st7735_draw_string(4, y, line, ST_CYAN, ST_BLACK, 1); y += 12;
    snprintf(line, sizeof(line), "%02X:%02X:%02X", mac[3], mac[4], mac[5]);
    st7735_draw_string(4, y, line, ST_CYAN, ST_BLACK, 1);

    ESP_LOGI(TAG, "ESP32 rev%d, %d cores, heap %u bytes",
             chip.revision, chip.cores, (unsigned)esp_get_free_heap_size());
}

// ---- BTN1: WiFi scan -------------------------------------------------------

static void screen_wifi_scan(void)
{
    header("WiFi scan...");
    body_clear();
    st7735_draw_string(4, 24, "Scanning...", ST_YELLOW, ST_BLACK, 1);

    ap_info_t aps[MAX_APS];
    int n = wifi_scan_run(aps, MAX_APS);

    char t[24];
    snprintf(t, sizeof(t), "WiFi: %d found", n);
    header(t);
    body_clear();

    if (n == 0) {
        st7735_draw_string(4, 24, "No networks", ST_RED, ST_BLACK, 1);
        return;
    }
    int y = HEADER_H + 4;
    for (int i = 0; i < n && y < ST7735_HEIGHT - 10; i++) {
        char line[26];
        // Trim SSID so it fits the 128px width (~20 chars at size 1).
        char ssid[16];
        strncpy(ssid, aps[i].ssid, sizeof(ssid) - 1);
        ssid[sizeof(ssid) - 1] = '\0';
        snprintf(line, sizeof(line), "%s", ssid);
        uint16_t col = (aps[i].rssi > -67) ? ST_GREEN
                     : (aps[i].rssi > -80) ? ST_YELLOW : ST_ORANGE;
        st7735_draw_string(2, y, line, col, ST_BLACK, 1);
        char meta[16];
        snprintf(meta, sizeof(meta), "%ddBm c%d", aps[i].rssi, aps[i].channel);
        st7735_draw_string(2, y + 8, meta, ST_GRAY, ST_BLACK, 1);
        y += 19;
    }
}

// ---- main ------------------------------------------------------------------

void app_main(void)
{
    ESP_LOGI(TAG, "LilyGO TTGO T10 V2.0 - ESP-IDF test firmware booting");

    power_init();          // best-effort IP5306 keep-on (battery use)
    st7735_init();
    buttons_init();
    sound_init();
    wifi_scan_init();

    // Boot splash image + cute melody; hold the image until a button is pressed.
    st7735_draw_image(0, 0, BOOT_IMAGE_W, BOOT_IMAGE_H, boot_image);
    st7735_fill_rect(0, 144, ST7735_WIDTH, 12, ST_BLACK);
    st7735_draw_string(20, 146, "press any key", ST_WHITE, ST_BLACK, 1);
    sound_play_boot_melody();

    buttons_poll();                                   // clear any stale edge
    while (buttons_poll() == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    screen_home();

    int64_t last_beat = 0;
    while (1) {
        int pressed = buttons_poll();
        if (pressed & (1 << BTN_1)) { screen_wifi_scan();  vTaskDelay(pdMS_TO_TICKS(2500)); screen_home(); }
        if (pressed & (1 << BTN_2)) { demo_colors();        screen_home(); }
        if (pressed & (1 << BTN_3)) { screen_board_info();  vTaskDelay(pdMS_TO_TICKS(2500)); screen_home(); }

        int64_t now = esp_timer_get_time();
        if (now - last_beat > 3 * 1000 * 1000) {   // every 3 s
            last_beat = now;
            ESP_LOGI(TAG, "alive: uptime %llus, free heap %u bytes",
                     (unsigned long long)(now / 1000000),
                     (unsigned)esp_get_free_heap_size());
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
