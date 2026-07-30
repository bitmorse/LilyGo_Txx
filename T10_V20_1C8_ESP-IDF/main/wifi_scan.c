#include "wifi_scan.h"

#include <string.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_log.h"

static const char *TAG = "wifi";

// WiFi is initialised and started by provisioning_init(); we only scan here.
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
