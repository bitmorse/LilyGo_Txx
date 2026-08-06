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
#include "netmgr.h"
#include "sound.h"
#include "imu.h"
#include "lvgl_port.h"
#include "ui_menu.h"
#include "settings.h"
#include "uartrx.h"
#include "sdcard.h"
#include "buttons.h"
#include "nvs_flash.h"
#include "boot_image.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "LilyGO TTGO T10 V2.0 - ESP-IDF test firmware booting");

    st7735_init();
    buttons_init();
    sound_init();
    imu_init();
    provisioning_hw_init();// NVS + netif + esp_wifi_init (no mode/start yet)
    settings_init();       // load user prefs (needs NVS, inited above)
    uartrx_init();         // GPIO21 -> REST (input, no pull); UART attached on demand

    // Boot splash image, and the cute melody only if enabled in Settings.
    st7735_draw_image(0, 0, BOOT_IMAGE_W, BOOT_IMAGE_H, boot_image);
    if (settings_boot_sound()) sound_play_boot_melody();
    vTaskDelay(pdMS_TO_TICKS(1500));

    // Hand the display to LVGL and build the menu.
    lvgl_port_init();
    lvgl_port_lock();
    ui_menu_start();
    lvgl_port_unlock();

    // Mount the SD card ONCE, here, after the UI is already up. Best-effort: a
    // failed/absent/corrupt card just logs and boot continues (esp_vfs_fat_sdspi_mount
    // returns an error rather than aborting, and format_if_mount_failed is off, so a
    // bad card cannot brick booting). Doing the one heavy mount from this task means
    // the on-demand consumers (uartrx, viblog, filesrv) hit sd_mount()'s early-out
    // instead of running the deep mount on their own smaller task stacks -- which is
    // what crashed the UART RX page when it had to mount the card itself.
    if (!sd_mount()) ESP_LOGW(TAG, "SD not mounted at boot (no card?); features mount on demand");

    // The connectivity state manager owns everything WiFi/BLE from here: it picks
    // the initial mode (STA / sync / provisioning) and arbitrates all transitions
    // and the SoftAP session lifecycle (see netmgr.c).
    netmgr_start();

    // app_main idles; LVGL runs in its own task. Keep a serial heartbeat.
    int64_t last_beat = 0, reset_since = 0;
    while (1) {
        int64_t now = esp_timer_get_time();

        // Factory reset: hold ENTER (BTN_1) + DOWN (BTN_3) for 5 s -> wipe ALL NVS
        // (BLE bonds + WiFi creds + settings) and reboot into S0. The only recovery
        // when the device is bonded to a phone that is gone (docs/DEVICE_STATE.md).
        if (buttons_level(BTN_1) && buttons_level(BTN_3)) {
            if (!reset_since) reset_since = now;
            else if (now - reset_since > 5 * 1000 * 1000) {
                ESP_LOGW(TAG, "FACTORY RESET (buttons held) -> erase NVS + reboot");
                nvs_flash_erase();
                esp_restart();
            }
        } else {
            reset_since = 0;
        }

        if (now - last_beat > 5 * 1000 * 1000) {   // every 5 s
            last_beat = now;
            char ip[16] = "-";
            esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ipi;
            if (sta && esp_netif_get_ip_info(sta, &ipi) == ESP_OK && ipi.ip.addr)
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ipi.ip));
            ESP_LOGI(TAG, "alive: uptime %llus, heap %u, state %s ip %s",
                     (unsigned long long)(now / 1000000),
                     (unsigned)esp_get_free_heap_size(),
                     netmgr_state_str(netmgr_state()), ip);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
