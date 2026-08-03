// LilyGO TTGO T10 V2.0 (1.8" ST7735) - ESP-IDF test firmware.
//
//   * boot splash image + chiptune melody, then an LVGL 9 UI
//   * 3 buttons act as an encoder for menu navigation:
//       BTN1 (GPIO35) = enter / select (encoder press)
//       BTN2 (GPIO34) = up   (rotate -)
//       BTN3 (GPIO39) = down (rotate +)
//   * settings menu: WiFi switch, backlight number, mode dropdown, and
//     actions (WiFi scan, sensors, board info, reboot)
//   * uptime/heap heartbeat over serial (`make monitor`)

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include "st7735.h"
#include "buttons.h"
#include "provisioning.h"
#include "power.h"
#include "sound.h"
#include "imu.h"
#include "lvgl_port.h"
#include "ui_menu.h"
#include "settings.h"
#include "blesync.h"
#include "boot_image.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "LilyGO TTGO T10 V2.0 - ESP-IDF test firmware booting");

    power_init();          // best-effort IP5306 keep-on (battery use)
    st7735_init();
    buttons_init();
    sound_init();
    imu_init();
    provisioning_init();   // NVS + WiFi + BLE provisioning (or auto-connect)
    settings_init();       // load user prefs (needs NVS, inited above)

    // Boot splash image, and the cute melody only if enabled in Settings.
    st7735_draw_image(0, 0, BOOT_IMAGE_W, BOOT_IMAGE_H, boot_image);
    if (settings_boot_sound()) sound_play_boot_melody();
    vTaskDelay(pdMS_TO_TICKS(1500));

    // Hand the display to LVGL and build the menu.
    lvgl_port_init();
    lvgl_port_lock();
    ui_menu_start();
    lvgl_port_unlock();

    // The SD card + file server are started ON DEMAND (File Sync / Vibration Log),
    // never at boot: a corrupt card must not be able to brick booting. Both modes
    // sd_mount() (idempotent) and neither unmounts, so they never race the mount.

    // In sync mode (no WiFi creds), start the BLE control channel so a phone can
    // discover the device, trigger SoftAP, and get the handoff.
    if (provisioning_sync_mode())
        blesync_start();

    // app_main idles; LVGL runs in its own task. Keep a serial heartbeat.
    int64_t last_beat = 0;
    while (1) {
        int64_t now = esp_timer_get_time();
        if (now - last_beat > 5 * 1000 * 1000) {   // every 5 s
            last_beat = now;
            wifi_ap_record_t ap;
            bool conn = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
            char ip[16] = "-";
            esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ipi;
            if (sta && esp_netif_get_ip_info(sta, &ipi) == ESP_OK && ipi.ip.addr)
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ipi.ip));
            ESP_LOGI(TAG, "alive: uptime %llus, heap %u, wifi %s rssi %d ip %s",
                     (unsigned long long)(now / 1000000),
                     (unsigned)esp_get_free_heap_size(),
                     conn ? "UP" : "down", conn ? ap.rssi : 0, ip);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
