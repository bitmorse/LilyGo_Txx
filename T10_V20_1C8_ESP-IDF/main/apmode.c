#include "apmode.h"
#include "provisioning.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "apmode";

#define AP_CHANNEL   1
#define AP_MAX_CONN  4

static esp_netif_t   *s_ap_netif;
static volatile bool  s_active;
static volatile int   s_clients;
static volatile int64_t s_last_client_ms;      // last time a client was present

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

bool apmode_active(void)  { return s_active; }
int  apmode_clients(void) { return s_clients; }

// Milliseconds with zero clients connected (0 while a client is present). Used by
// the sync-session watchdog to tear the AP down if nobody uses it.
int64_t apmode_no_client_ms(void)
{
    if (!s_active || s_clients > 0) return 0;
    return now_ms() - s_last_client_ms;
}

static void ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_AP_STACONNECTED)    { s_clients++; s_last_client_ms = now_ms(); ESP_LOGI(TAG, "client joined (%d)", s_clients); }
    else if (id == WIFI_EVENT_AP_STADISCONNECTED) { if (s_clients > 0) s_clients--; s_last_client_ms = now_ms(); ESP_LOGI(TAG, "client left (%d)", s_clients); }
}

static void make_ssid(char *out, int cap)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, cap, "Octanis-%02X%02X", mac[4], mac[5]);
}

// DEV: a fixed WPA2 passphrase so the test laptop remembers the SoftAP and we
// don't retype it each boot. Swap back to a random per-session passphrase
// (delivered over the encrypted BLE handoff) before release (task #25).
static void make_pass(char *out, int cap)
{
    snprintf(out, cap, "octanis123");
}

bool apmode_start(char *ssid, int ssid_cap, char *pass, int pass_cap)
{
    make_ssid(ssid, ssid_cap);                 // deterministic; safe to recompute
    make_pass(pass, pass_cap);
    if (s_active) return true;                  // already up -- don't re-blip the AP
                                                // (a re-triggered START_SOFTAP would
                                                // otherwise drop the client mid-download)

    // SoftAP + active BLE advertising is unstable on the classic ESP32 (the BLE
    // events interrupt the Wi-Fi transfer -> mid-stream drops). Stop provisioning
    // BLE before bringing the AP up. The manager stop is ASYNC and its teardown
    // flips Wi-Fi back to STA + releases BT ~1 s later, which would clobber our AP
    // if we set AP mode first -- so wait it out here (this runs off the UI task).
    provisioning_stop_ble();
    vTaskDelay(pdMS_TO_TICKS(1500));

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, ap_event, NULL);
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, ap_event, NULL);
    }

    wifi_config_t apcfg = {
        .ap = {
            .channel        = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg        = { .required = false },
        },
    };
    snprintf((char *)apcfg.ap.ssid, sizeof(apcfg.ap.ssid), "%s", ssid);
    apcfg.ap.ssid_len = strlen((char *)apcfg.ap.ssid);
    snprintf((char *)apcfg.ap.password, sizeof(apcfg.ap.password), "%s", pass);

    s_clients = 0;
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK) return false;
    if (esp_wifi_set_config(WIFI_IF_AP, &apcfg) != ESP_OK) return false;
    esp_wifi_start();                          // no-op if already started
    esp_wifi_set_ps(WIFI_PS_NONE);             // responsive server

    s_last_client_ms = now_ms();               // start the no-client teardown clock
    s_active = true;
    ESP_LOGI(TAG, "SoftAP up: SSID=%s pass=%s  http://192.168.4.1:8080", ssid, pass);
    return true;
}

void apmode_stop(void)
{
    if (!s_active) return;
    s_active = false;
    s_clients = 0;
    // Back to STA and rejoin the provisioned home network.
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_connect();
    ESP_LOGI(TAG, "SoftAP down, reconnecting to home Wi-Fi");
}
