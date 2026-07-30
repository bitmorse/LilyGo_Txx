// LilyGO TTGO T10 V2.0 (1.8" ST7735) - ESP-IDF test firmware.
//
//   * boot splash image + chiptune melody, then an LVGL 9 UI
//   * 3 buttons act as an encoder for menu navigation:
//       BTN1 (GPIO35) = back / up      (rotate -)
//       BTN2 (GPIO34) = forward / down (rotate +)
//       BTN3 (GPIO39) = select / edit  (encoder press)
//   * settings menu: WiFi switch, backlight number, mode dropdown, and
//     actions (WiFi scan, sensors, board info, reboot)
//   * uptime/heap heartbeat over serial (`make monitor`)

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "st7735.h"
#include "buttons.h"
#include "wifi_scan.h"
#include "provisioning.h"
#include "power.h"
#include "sound.h"
#include "imu.h"
#include "i2c_bus.h"
#include "lvgl_port.h"
#include "ui_menu.h"
#include "boot_image.h"

static const char *TAG = "app";

// Result of the boot-time I2C pin sweep (logged; also handy for diagnostics).
static char g_i2c_sweep[256];

void app_main(void)
{
    ESP_LOGI(TAG, "LilyGO TTGO T10 V2.0 - ESP-IDF test firmware booting");

    // Diagnostic: sweep candidate I2C pins BEFORE any peripheral claims them
    // (the board's header pins for I2C proved wrong).
    i2c_bus_sweep_pins(g_i2c_sweep, sizeof(g_i2c_sweep));

    power_init();          // best-effort IP5306 keep-on (battery use)
    st7735_init();
    buttons_init();
    sound_init();
    imu_init();
    provisioning_init();   // NVS + WiFi + BLE provisioning (or auto-connect)

    // Boot splash image + cute melody, drawn directly before LVGL takes over.
    st7735_draw_image(0, 0, BOOT_IMAGE_W, BOOT_IMAGE_H, boot_image);
    sound_play_boot_melody();
    vTaskDelay(pdMS_TO_TICKS(1500));

    // Hand the display to LVGL and build the menu.
    lvgl_port_init();
    lvgl_port_lock();
    ui_menu_start();
    lvgl_port_unlock();

    // app_main idles; LVGL runs in its own task. Keep a serial heartbeat.
    int64_t last_beat = 0;
    while (1) {
        int64_t now = esp_timer_get_time();
        if (now - last_beat > 5 * 1000 * 1000) {   // every 5 s
            last_beat = now;
            ESP_LOGI(TAG, "alive: uptime %llus, free heap %u bytes",
                     (unsigned long long)(now / 1000000),
                     (unsigned)esp_get_free_heap_size());
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
