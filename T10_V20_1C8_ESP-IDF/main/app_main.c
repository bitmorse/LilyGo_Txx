// LilyGO TTGO T10 V2.0 (1.8" ST7735) - ESP-IDF test firmware.
//
//   * splash + graphics demo on the TFT
//   * 3 onboard buttons drive a tiny menu
//       BTN1 (GPIO35) -> WiFi scan, results on screen + serial
//       BTN2 (GPIO34) -> live MPU9250 sensor readout (accel/gyro/temp/mag)
//       BTN3 (GPIO39) -> board info (chip, flash, heap, MAC)
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
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"

#include "st7735.h"
#include "buttons.h"
#include "wifi_scan.h"
#include "power.h"
#include "sound.h"
#include "imu.h"
#include "i2c_bus.h"
#include "boot_image.h"

static const char *TAG = "app";

#define HEADER_H 14
#define MAX_APS  8

// Result of the boot-time I2C pin sweep (shown on the Sensors page if no IMU).
static char g_i2c_sweep[256];

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
    st7735_draw_string(4, 72,  "BTN2: Sensors",    ST_YELLOW, ST_BLACK, 1);
    st7735_draw_string(4, 86,  "BTN3: Board info", ST_CYAN,  ST_BLACK, 1);

    st7735_draw_string(4, 120, "Press a button", ST_GRAY, ST_BLACK, 1);
    st7735_draw_string(4, 130, "to start...",    ST_GRAY, ST_BLACK, 1);
}

// ---- BTN2: live sensor readout (MPU9250 IMU) ------------------------------

static void screen_imu(void)
{
    header("MPU9250 IMU");
    body_clear();

    imu_data_t d;
    if (!imu_read(&d)) {
        st7735_draw_string(4, 18, "IMU not found", ST_RED, ST_BLACK, 1);
        st7735_draw_string(4, 34, "I2C pin sweep:", ST_GRAY, ST_BLACK, 1);
        // g_i2c_sweep holds newline-separated "SDAx SCLy:NN" hits (or a note).
        st7735_draw_string(2, 48, g_i2c_sweep, ST_CYAN, ST_BLACK, 1);
        st7735_draw_string(4, 130, "any key: back", ST_GRAY, ST_BLACK, 1);
        buttons_poll();
        while (buttons_poll() == 0) vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }

    // Static labels; only the value rows are redrawn each frame.
    st7735_draw_string(4, 18,  "Accel  mg",  ST_GRAY, ST_BLACK, 1);
    st7735_draw_string(4, 44,  "Gyro   dps", ST_GRAY, ST_BLACK, 1);
    st7735_draw_string(4, 70,  "Mag    raw", ST_GRAY, ST_BLACK, 1);
    st7735_draw_string(4, 96,  "Temp",       ST_GRAY, ST_BLACK, 1);
    st7735_draw_string(4, 130, "any key: back", ST_GRAY, ST_BLACK, 1);

    buttons_poll();   // clear the edge that opened this page
    char l[32];
    while (buttons_poll() == 0) {
        if (imu_read(&d)) {
            snprintf(l, sizeof(l), "%+5d %+5d %+5d",
                     (int)(d.ax * 1000), (int)(d.ay * 1000), (int)(d.az * 1000));
            st7735_draw_string(4, 30, l, ST_CYAN, ST_BLACK, 1);

            snprintf(l, sizeof(l), "%+5d %+5d %+5d",
                     (int)d.gx, (int)d.gy, (int)d.gz);
            st7735_draw_string(4, 56, l, ST_GREEN, ST_BLACK, 1);

            if (d.mag_ok)
                snprintf(l, sizeof(l), "%+5d %+5d %+5d", d.mx, d.my, d.mz);
            else
                snprintf(l, sizeof(l), "  n/a           ");
            st7735_draw_string(4, 82, l, ST_YELLOW, ST_BLACK, 1);

            int t = (int)(d.temp_c * 10);
            snprintf(l, sizeof(l), "%d.%d C  ", t / 10, (t < 0 ? -t : t) % 10);
            st7735_draw_string(46, 96, l, ST_WHITE, ST_BLACK, 1);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
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

    // Total flash size.
    uint32_t flash_total = 0;
    esp_flash_get_size(NULL, &flash_total);

    // Unpartitioned flash = total minus the highest partition end.
    size_t part_end = 0;
    for (esp_partition_iterator_t it =
             esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
         it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p->address + p->size > part_end) part_end = p->address + p->size;
    }
    uint32_t flash_free = (flash_total > part_end) ? (flash_total - part_end) : 0;

    size_t ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);

    char line[32];
    int y = 18;

    snprintf(line, sizeof(line), "ESP32 rev%d %dc", chip.revision, chip.cores);
    st7735_draw_string(4, y, line, ST_WHITE, ST_BLACK, 1); y += 13;

    snprintf(line, sizeof(line), "Flash: %uMB", (unsigned)(flash_total >> 20));
    st7735_draw_string(4, y, line, ST_YELLOW, ST_BLACK, 1); y += 13;

    snprintf(line, sizeof(line), " free: %uKB", (unsigned)(flash_free / 1024));
    st7735_draw_string(4, y, line, ST_YELLOW, ST_BLACK, 1); y += 13;

    snprintf(line, sizeof(line), "RAM: %uKB", (unsigned)(ram_total / 1024));
    st7735_draw_string(4, y, line, ST_WHITE, ST_BLACK, 1); y += 13;

    snprintf(line, sizeof(line), " heap: %uKB", (unsigned)(esp_get_free_heap_size() / 1024));
    st7735_draw_string(4, y, line, ST_GREEN, ST_BLACK, 1); y += 13;

    snprintf(line, sizeof(line), " min:  %uKB",
             (unsigned)(esp_get_minimum_free_heap_size() / 1024));
    st7735_draw_string(4, y, line, ST_GREEN, ST_BLACK, 1); y += 13;

    st7735_draw_string(4, y, "MAC:", ST_GRAY, ST_BLACK, 1); y += 12;
    snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    st7735_draw_string(4, y, line, ST_CYAN, ST_BLACK, 1);

    ESP_LOGI(TAG, "ESP32 rev%d %dc | flash %uMB (free %uKB) | RAM %uKB heap-free %uKB",
             chip.revision, chip.cores, (unsigned)(flash_total >> 20),
             (unsigned)(flash_free / 1024), (unsigned)(ram_total / 1024),
             (unsigned)(esp_get_free_heap_size() / 1024));
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

    // Diagnostic: sweep candidate I2C pins BEFORE any peripheral claims them
    // (the board's header pins for I2C proved wrong). Result shown on Sensors.
    i2c_bus_sweep_pins(g_i2c_sweep, sizeof(g_i2c_sweep));

    power_init();          // best-effort IP5306 keep-on (battery use)
    st7735_init();
    buttons_init();
    sound_init();
    imu_init();
    wifi_scan_init();

    // Boot splash image + cute melody; hold the image until a button is pressed.
    st7735_draw_image(0, 0, BOOT_IMAGE_W, BOOT_IMAGE_H, boot_image);
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
        if (pressed & (1 << BTN_2)) { screen_imu();         screen_home(); }
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
