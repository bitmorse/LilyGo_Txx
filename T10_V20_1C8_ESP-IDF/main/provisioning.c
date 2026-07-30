#include "provisioning.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

static const char *TAG = "prov";

#define PROV_POP     "t10setup"       // proof-of-possession the app must enter
#define MAX_RETRY    5

static prov_state_t s_state = PROV_IDLE;
static char s_service_name[20] = "T10_????";
static char s_qr[96];
static int  s_retries = 0;

prov_state_t provisioning_state(void)        { return s_state; }
const char  *provisioning_service_name(void) { return s_service_name; }
const char  *provisioning_pop(void)          { return PROV_POP; }
const char  *provisioning_qr_payload(void)   { return s_qr; }

// --- event handling ---------------------------------------------------------

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_PROV_EVENT) {
        switch (id) {
        case WIFI_PROV_START:
            s_state = PROV_PAIRING;
            ESP_LOGI(TAG, "BLE provisioning started as '%s' (PoP '%s')",
                     s_service_name, PROV_POP);
            break;
        case WIFI_PROV_CRED_RECV:
            s_state = PROV_CONNECTING;
            ESP_LOGI(TAG, "credentials received, connecting...");
            break;
        case WIFI_PROV_CRED_FAIL:
            s_state = PROV_FAILED;
            ESP_LOGW(TAG, "provisioning failed (bad password or AP not found)");
            break;
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "provisioning successful");
            break;
        case WIFI_PROV_END:
            wifi_prov_mgr_deinit();    // releases BLE (FREE_BTDM scheme handler)
            break;
        default:
            break;
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Cap TX power (~13 dBm) to reduce current spikes that can brown out
        // this board on USB with the display backlight on.
        esp_wifi_set_max_tx_power(52);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries++ < MAX_RETRY) {
            esp_wifi_connect();
        } else {
            s_state = PROV_FAILED;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retries = 0;
        s_state = PROV_CONNECTED;
        ESP_LOGI(TAG, "WiFi connected, got IP");
    }
}

static void make_service_name(void)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_service_name, sizeof(s_service_name), "T10_%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    // Payload the ESP BLE Provisioning app expects from a QR scan.
    snprintf(s_qr, sizeof(s_qr),
             "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\"}",
             s_service_name, PROV_POP);
}

// --- public -----------------------------------------------------------------

void provisioning_init(void)
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

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                               event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               event_handler, NULL));

    make_service_name();

    // Provisioning manager, BLE transport. FREE_BTDM releases the BT/BLE memory
    // once provisioning ends, reclaiming RAM for normal operation.
    wifi_prov_mgr_config_t mgr = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(mgr));

    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (!provisioned) {
        ESP_LOGI(TAG, "not provisioned -> starting BLE pairing");
        s_state = PROV_PAIRING;
        wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
            security, PROV_POP, s_service_name, NULL));
        // manager runs in the background; it releases BLE when done.
    } else {
        ESP_LOGI(TAG, "already provisioned -> connecting");
        wifi_prov_mgr_deinit();
        s_state = PROV_CONNECTING;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());   // STA_START handler connects
    }
}

// Run the WiFi teardown + reboot in a dedicated task: it must NOT run in the
// LVGL event callback (that task's stack is small and it holds the LVGL mutex).
static void reset_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "erasing WiFi credentials, rebooting into pairing mode");
    esp_wifi_restore();          // clears stored STA config from NVS
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

void provisioning_reset_and_restart(void)
{
    xTaskCreate(reset_task, "prov_reset", 4096, NULL, 5, NULL);
}
