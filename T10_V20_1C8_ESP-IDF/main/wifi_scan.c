#include "wifi_scan.h"

#include <string.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "wifi";

void wifi_scan_init(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi station started");
}

int wifi_scan_run(ap_info_t *out, int max)
{
    wifi_scan_config_t sc = { .show_hidden = true };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed");
        return 0;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) return 0;

    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (!recs) return 0;
    esp_wifi_scan_get_ap_records(&num, recs);

    int count = (num > max) ? max : num;
    for (int i = 0; i < count; i++) {
        strncpy(out[i].ssid, (const char *)recs[i].ssid, sizeof(out[i].ssid) - 1);
        out[i].ssid[sizeof(out[i].ssid) - 1] = '\0';
        if (out[i].ssid[0] == '\0') strcpy(out[i].ssid, "<hidden>");
        out[i].rssi    = recs[i].rssi;
        out[i].channel = recs[i].primary;
        ESP_LOGI(TAG, "  %2d ch%-2d %4d dBm  %s",
                 i, out[i].channel, out[i].rssi, out[i].ssid);
    }
    free(recs);
    return count;
}
