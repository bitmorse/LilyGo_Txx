#include "settings.h"

#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";

#define NS          "settings"
#define K_BOOT_SND  "boot_snd"
#define K_WLAN_MODE "wlan_mode"

static bool s_boot_sound;      // cached; default off (quiet boot)
static bool s_wlan_mode = true;// cached; default WLAN

static void save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

void settings_init(void)
{
    s_boot_sound = false;                          // defaults
    s_wlan_mode  = true;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, K_BOOT_SND, &v) == ESP_OK)  s_boot_sound = (v != 0);
        if (nvs_get_u8(h, K_WLAN_MODE, &v) == ESP_OK) s_wlan_mode  = (v != 0);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "boot_sound=%d wlan_mode=%d", s_boot_sound, s_wlan_mode);
}

bool settings_boot_sound(void) { return s_boot_sound; }
bool settings_wlan_mode(void)  { return s_wlan_mode; }

void settings_set_boot_sound(bool on)  { s_boot_sound = on;   save_u8(K_BOOT_SND, on ? 1 : 0); }
void settings_set_wlan_mode(bool wlan) { s_wlan_mode = wlan;  save_u8(K_WLAN_MODE, wlan ? 1 : 0); }
