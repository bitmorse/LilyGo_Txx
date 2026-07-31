#include "settings.h"

#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";

#define NS          "settings"
#define K_BOOT_SND  "boot_snd"

static bool s_boot_sound;      // cached; default off (quiet boot)

void settings_init(void)
{
    s_boot_sound = false;                          // default
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, K_BOOT_SND, &v) == ESP_OK) s_boot_sound = (v != 0);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "boot_sound=%d", s_boot_sound);
}

bool settings_boot_sound(void) { return s_boot_sound; }

void settings_set_boot_sound(bool on)
{
    s_boot_sound = on;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, K_BOOT_SND, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}
