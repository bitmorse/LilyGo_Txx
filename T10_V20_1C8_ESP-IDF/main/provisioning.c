#include "provisioning.h"
#include "netmgr.h"

#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "prov";

static char s_service_name[20] = "T10_????";
static bool s_connected;             // mirror of netmgr's STA-connected state

// --- getters ----------------------------------------------------------------

const char *provisioning_service_name(void) { return s_service_name; }

void provisioning_set_connected(bool on) { s_connected = on; }
bool provisioning_is_connected(void)     { return s_connected; }

void provisioning_ssid(char *buf, int n)
{
    wifi_config_t cfg;
    if (n <= 0) return;
    buf[0] = '\0';
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK)
        snprintf(buf, n, "%s", (char *)cfg.sta.ssid);
}

int provisioning_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        return ap.rssi;
    return 0;
}

// --- event handling: forward to netmgr, make no policy decisions ------------

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Cap TX power (~13 dBm) to reduce current spikes that can brown out this
        // board on USB with the backlight on. Do NOT connect here -- netmgr does.
        esp_wifi_set_max_tx_power(52);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        netmgr_post_event(NETEV_STA_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi connected, got IP");
        netmgr_post_event(NETEV_STA_GOT_IP);
    }
}

// --- wall-clock time --------------------------------------------------------

#define TIME_SYNCED_EPOCH 1700000000ULL   // 2023-11-14; below this the RTC is unset

bool time_is_synced(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec >= TIME_SYNCED_EPOCH;
}

uint64_t time_now_ns(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if ((uint64_t)tv.tv_sec >= TIME_SYNCED_EPOCH)
        return (uint64_t)tv.tv_sec * 1000000000ULL + (uint64_t)tv.tv_usec * 1000ULL;
    return (uint64_t)esp_timer_get_time() * 1000ULL;   // monotonic fallback
}

// Set the wall clock from a phone-supplied UTC time (BLE time-sync, no WiFi needed).
// ms = Unix epoch milliseconds. Rejects anything before TIME_SYNCED_EPOCH so a bad
// write can't push the RTC back into the "unsynced" range. Returns true on success.
bool time_set_unix_ms(int64_t ms)
{
    if (ms < (int64_t)(TIME_SYNCED_EPOCH * 1000ULL)) return false;   // sanity: after 2023
    struct timeval tv = { .tv_sec = (time_t)(ms / 1000), .tv_usec = (suseconds_t)((ms % 1000) * 1000) };
    return settimeofday(&tv, NULL) == 0;
}

void provisioning_start_sntp(void)
{
    static bool started = false;
    if (started) return;
    started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
}

static void make_service_name(void)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_service_name, sizeof(s_service_name), "T10_%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

// --- hardware init ----------------------------------------------------------

void provisioning_hw_init(void)
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

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,      ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,     event_handler, NULL));

    make_service_name();
    // Set STA mode (so the stored STA config + service-name MAC are readable and the
    // STA netif is ready) but do NOT start the radio -- netmgr starts it in the mode
    // the chosen state needs (STA connect, or AP for SoftAP). No start() => no
    // connect attempts, so no "no suitable AP" churn while idle in sync mode.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
}

// --- STA mechanism ----------------------------------------------------------

bool provisioning_has_creds(void)
{
    wifi_config_t cfg = {0};
    // esp_wifi_get_config needs WiFi in a started-ish state on some paths; the STA
    // config is readable after esp_wifi_init, which hw_init already did.
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
    return cfg.sta.ssid[0] != '\0';
}

void provisioning_sta_connect(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_wifi_start();                 // no-op if already started; fires STA_START
    esp_wifi_connect();
}

void provisioning_sta_reconnect(void) { esp_wifi_connect(); }
void provisioning_sta_disconnect(void) { esp_wifi_disconnect(); }

// --- credentials (written over BLE by the app; verified by netmgr) ------------

bool provisioning_store_creds(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') return false;
    wifi_config_t cfg = {0};
    snprintf((char *)cfg.sta.ssid,     sizeof(cfg.sta.ssid),     "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", pass ? pass : "");
    // Leave authmode threshold at 0 (OPEN): a non-empty password still brings up WPA2;
    // an empty one connects to an open AP. esp_wifi persists this to NVS (FLASH storage).
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(e)); return false; }
    ESP_LOGI(TAG, "stored candidate creds for '%s'", ssid);
    return true;
}

void provisioning_forget(void)
{
    esp_wifi_restore();               // clears stored STA config from NVS
}
