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
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

static const char *TAG = "prov";

#define PROV_POP     "t10setup"       // proof-of-possession the app must enter
#define NVS_NS       "netmgr"
#define NVS_KEY_PEND "provpend"       // 1 = boot straight into BLE provisioning

static char s_service_name[20] = "T10_????";
static bool s_connected;             // mirror of netmgr's STA-connected state

// --- getters ----------------------------------------------------------------

const char *provisioning_service_name(void) { return s_service_name; }
const char *provisioning_pop(void)          { return PROV_POP; }

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
    if (base == WIFI_PROV_EVENT) {
        switch (id) {
        case WIFI_PROV_START:        ESP_LOGI(TAG, "BLE provisioning started '%s'", s_service_name); break;
        case WIFI_PROV_CRED_RECV:    ESP_LOGI(TAG, "provisioning: credentials received"); break;
        case WIFI_PROV_CRED_FAIL:    ESP_LOGW(TAG, "provisioning: bad password / AP not found");
                                     netmgr_post_event(NETEV_PROV_FAIL); break;
        case WIFI_PROV_CRED_SUCCESS: ESP_LOGI(TAG, "provisioning: success");
                                     netmgr_post_event(NETEV_PROV_SUCCESS); break;
        case WIFI_PROV_END:          wifi_prov_mgr_deinit(); break;
        default: break;
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
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

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
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

// --- BLE WiFi-provisioning mechanism ----------------------------------------

bool provisioning_prov_start(void)
{
    wifi_prov_mgr_config_t config = {
        .scheme               = wifi_prov_scheme_ble,
        // FREE_BTDM frees the BT stack when provisioning ends. Safe here: entering
        // and leaving PROVISIONING both go through a reboot (netmgr), so BLE is
        // never needed again in this boot.
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    // NO ESP_ERROR_CHECK: a failure here must NOT abort -- prov_pending is still set,
    // so an abort would reboot straight back into this failing path (a reboot loop).
    // Return false and let netmgr clear the flag and fall back.
    esp_err_t err = wifi_prov_mgr_init(config);
    if (err != ESP_OK) { ESP_LOGE(TAG, "prov_mgr_init: %s", esp_err_to_name(err)); return false; }

    // Distinct 128-bit service UUID for the provisioning GATT service.
    uint8_t uuid[16] = { 0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
                         0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02 };
    wifi_prov_scheme_ble_set_service_uuid(uuid);
    err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, PROV_POP,
                                           s_service_name, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start_provisioning: %s", esp_err_to_name(err));
        wifi_prov_mgr_deinit();
        return false;
    }
    return true;
}

void provisioning_prov_stop(void)
{
    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();
}

// --- persistence ------------------------------------------------------------

bool provisioning_prov_pending(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_PEND, &v);
        nvs_close(h);
    }
    return v != 0;
}

void provisioning_set_prov_pending(bool on)
{
    nvs_handle_t h;
    esp_err_t eo = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (eo != ESP_OK) { ESP_LOGE(TAG, "prov_pending open: %s", esp_err_to_name(eo)); return; }
    esp_err_t es = nvs_set_u8(h, NVS_KEY_PEND, on ? 1 : 0);
    esp_err_t ec = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "prov_pending := %d (set=%s commit=%s)", on,
             esp_err_to_name(es), esp_err_to_name(ec));
}

void provisioning_forget(void)
{
    esp_wifi_restore();               // clears stored STA config from NVS
}
