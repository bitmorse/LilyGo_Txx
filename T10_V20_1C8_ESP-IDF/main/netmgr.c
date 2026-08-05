#include "netmgr.h"
#include "provisioning.h"
#include "blesync.h"
#include "apmode.h"
#include "filesrv.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "netmgr";

// Requests + events funnel through one queue so every transition runs on the single
// manager task -- no locking, no re-entrancy, no two callers switching modes at once.
typedef enum {
    MSG_SOFTAP_START,
    MSG_SOFTAP_STOP,
    MSG_PROVISION_CREDS,   // app wrote WIFI_CREDS (creds in s_pend_ssid/s_pend_pass)
    MSG_FORGET,
    MSG_STA_GOT_IP,
    MSG_STA_DISCONNECTED,
} msg_t;

#define STA_MAX_RETRY      6           // fast retries before declaring STA_FAILED
#define STA_REFAIL_MS      30000       // then a slow periodic re-attempt from failed
#define SOFTAP_NOCLIENT_MS 120000      // no client joined -> tear the AP down
#define SOFTAP_IDLE_MS     300000      // no HTTP traffic -> tear the AP down
#define TICK_MS            2000        // watchdog cadence

static net_state_t   s_state = NET_BOOT;
static QueueHandle_t s_q;
static int           s_sta_retries;
static bool          s_softap_from_sync;   // remember the base mode to return to
static int64_t       s_sta_refail_ms;      // next slow STA re-attempt (ms uptime)
static char          s_pend_ssid[33], s_pend_pass[65];   // creds awaiting verification

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

net_state_t netmgr_state(void)       { return s_state; }
bool        netmgr_internet_up(void) { return s_state == NET_STA_CONNECTED; }

const char *netmgr_state_str(net_state_t s)
{
    switch (s) {
    case NET_BOOT:           return "boot";
    case NET_STA_CONNECTING: return "sta-connecting";
    case NET_STA_CONNECTED:  return "sta-connected";
    case NET_STA_FAILED:     return "sta-failed";
    case NET_SYNC_IDLE:      return "sync-idle";
    case NET_SOFTAP:         return "softap";
    case NET_VERIFYING:      return "verifying";
    default:                 return "?";
    }
}

static void post(msg_t m)
{
    // Bounded wait, never portMAX_DELAY: post() runs in WiFi/BLE event-task context,
    // and the manager task can be busy in a blocking transition (blesync_stop ~1 s).
    if (s_q && xQueueSend(s_q, &m, pdMS_TO_TICKS(500)) != pdTRUE)
        ESP_LOGW(TAG, "queue full; dropped msg %d", (int)m);
}

void netmgr_request_softap(void)      { post(MSG_SOFTAP_START); }
void netmgr_request_stop_softap(void) { post(MSG_SOFTAP_STOP); }
void netmgr_request_forget_wifi(void) { post(MSG_FORGET); }

void netmgr_request_provision(const char *ssid, const char *pass)
{
    snprintf(s_pend_ssid, sizeof(s_pend_ssid), "%s", ssid ? ssid : "");
    snprintf(s_pend_pass, sizeof(s_pend_pass), "%s", pass ? pass : "");
    post(MSG_PROVISION_CREDS);
}

void netmgr_post_event(net_event_t ev)
{
    switch (ev) {
    case NETEV_STA_GOT_IP:       post(MSG_STA_GOT_IP); break;
    case NETEV_STA_DISCONNECTED: post(MSG_STA_DISCONNECTED); break;
    }
}

// --- transitions (all run on the manager task) ------------------------------
// blesync (the BLE control channel) is up in EVERY state except NET_SOFTAP -- Stage 0
// showed BLE + STA coexist fine (~50 KB free), but BLE + the HTTP file server do not.

static void enter_sta(void)
{
    s_state = NET_STA_CONNECTING;
    s_sta_retries = 0;
    provisioning_set_connected(false);
    blesync_start();                            // control channel alongside STA
    provisioning_sta_connect();                 // set STA mode + start + connect
    ESP_LOGI(TAG, "-> sta-connecting");
}

static void enter_sync(void)
{
    s_state = NET_SYNC_IDLE;
    provisioning_set_connected(false);
    blesync_start();
    ESP_LOGI(TAG, "-> sync-idle (BLE advertising)");
}

// App wrote WIFI_CREDS: store as candidate and try to join. Only GOT_IP commits it
// (-> sta-connected/provisioned); repeated failure discards it and returns to sync.
static void enter_verifying(void)
{
    if (!provisioning_store_creds(s_pend_ssid, s_pend_pass)) {
        blesync_notify_prov_result(false, "bad credentials");
        return;                                 // stay where we are
    }
    s_state = NET_VERIFYING;
    s_sta_retries = 0;
    provisioning_set_connected(false);
    blesync_start();
    provisioning_sta_connect();
    ESP_LOGI(TAG, "-> verifying '%s'", s_pend_ssid);
}

static void enter_softap(void)
{
    s_softap_from_sync = (s_state == NET_SYNC_IDLE);
    if (!s_softap_from_sync) provisioning_sta_disconnect();   // drop STA first

    if (!apmode_start_session() || !filesrv_start()) {
        ESP_LOGE(TAG, "softap start failed");
        apmode_stop();
        if (s_softap_from_sync) enter_sync(); else enter_sta();
        return;
    }
    // blesync is up in both bases now: hand the phone the creds+token over BLE, then
    // free BLE for the transfer (BLE + SoftAP together starve the heap).
    blesync_notify_handoff();
    vTaskDelay(pdMS_TO_TICKS(1000));            // let the notify reach the phone
    blesync_stop();
    s_state = NET_SOFTAP;
    ESP_LOGI(TAG, "-> softap (base=%s)", s_softap_from_sync ? "sync" : "sta");
}

static void exit_softap(void)
{
    filesrv_stop();
    apmode_stop();
    if (s_softap_from_sync) enter_sync();       // both restart blesync
    else                    enter_sta();
}

// --- message handling -------------------------------------------------------

static void handle(msg_t m)
{
    switch (m) {
    case MSG_SOFTAP_START:
        if (s_state == NET_SYNC_IDLE || s_state == NET_STA_CONNECTED ||
            s_state == NET_STA_CONNECTING || s_state == NET_STA_FAILED)
            enter_softap();
        break;

    case MSG_SOFTAP_STOP:
        if (s_state == NET_SOFTAP) exit_softap();
        break;

    case MSG_PROVISION_CREDS:
        if (s_state != NET_SOFTAP) enter_verifying();   // not while mid-transfer
        break;

    case MSG_FORGET:                             // live: erase creds -> sync-idle (no reboot)
        ESP_LOGW(TAG, "forget WiFi -> sync-idle");
        provisioning_sta_disconnect();
        provisioning_forget();
        enter_sync();
        break;

    case MSG_STA_GOT_IP:
        if (s_state == NET_VERIFYING) {
            blesync_notify_prov_result(true, NULL);      // provisioned + connected
            ESP_LOGI(TAG, "verified -> provisioned");
        }
        if (s_state == NET_VERIFYING || s_state == NET_STA_CONNECTING ||
            s_state == NET_STA_FAILED) {
            s_state = NET_STA_CONNECTED;
            s_sta_retries = 0;
            provisioning_set_connected(true);
            provisioning_start_sntp();
            ESP_LOGI(TAG, "-> sta-connected");
        }
        break;

    case MSG_STA_DISCONNECTED:
        if (s_state == NET_VERIFYING) {          // candidate creds not confirmed
            if (s_sta_retries++ < STA_MAX_RETRY) {
                provisioning_sta_reconnect();
            } else {
                ESP_LOGW(TAG, "verify failed -> discard creds, back to sync");
                provisioning_forget();
                blesync_notify_prov_result(false, "wrong password or AP not found");
                enter_sync();
            }
        } else if (s_state == NET_STA_CONNECTING || s_state == NET_STA_CONNECTED) {
            provisioning_set_connected(false);
            if (s_sta_retries++ < STA_MAX_RETRY) {
                s_state = NET_STA_CONNECTING;
                provisioning_sta_reconnect();
            } else {
                s_state = NET_STA_FAILED;
                s_sta_refail_ms = now_ms() + STA_REFAIL_MS;
                ESP_LOGW(TAG, "-> sta-failed (%d retries); slow retry in %d s",
                         s_sta_retries, STA_REFAIL_MS / 1000);
            }
        }
        break;
    }
}

static void tick(void)
{
    if (s_state == NET_SOFTAP && apmode_active() &&
        (apmode_no_client_ms() > SOFTAP_NOCLIENT_MS ||
         filesrv_idle_ms()     > SOFTAP_IDLE_MS)) {
        ESP_LOGW(TAG, "SoftAP unused -> teardown");
        exit_softap();
    }
    // A provisioned device that couldn't join keeps retrying slowly instead of
    // staying offline (WiFi may be briefly down).
    if (s_state == NET_STA_FAILED && now_ms() > s_sta_refail_ms) {
        s_state = NET_STA_CONNECTING;
        s_sta_retries = 0;
        ESP_LOGI(TAG, "sta-failed: slow reconnect attempt");
        provisioning_sta_reconnect();
    }
}

static void netmgr_task(void *arg)
{
    (void)arg;

    // Initial mode, decided once at boot -- no prov_pending, no reboot branch.
    bool creds = provisioning_has_creds();
    ESP_LOGI(TAG, "boot: has_creds=%d", creds);
    if (creds) enter_sta();
    else       enter_sync();

    for (;;) {
        msg_t m;
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) handle(m);
        else                                                         tick();
    }
}

void netmgr_start(void)
{
    if (s_q) return;
    s_q = xQueueCreate(8, sizeof(msg_t));
    // 6144: enter_softap() runs filesrv_start() (sd_mount FAT + httpd_start + mdns)
    // and blesync init/deinit on this task -- a deep chain for 4096.
    xTaskCreate(netmgr_task, "netmgr", 6144, NULL, 6, NULL);
}
