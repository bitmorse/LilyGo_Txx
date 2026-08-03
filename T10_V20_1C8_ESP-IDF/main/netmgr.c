#include "netmgr.h"
#include "provisioning.h"
#include "blesync.h"
#include "apmode.h"
#include "filesrv.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "netmgr";

// Requests + events funnel through one queue so every transition runs on the single
// manager task -- no locking, no re-entrancy, no two callers switching modes at once.
typedef enum {
    MSG_SOFTAP_START,
    MSG_SOFTAP_STOP,
    MSG_PROVISION,
    MSG_FORGET,
    MSG_STA_GOT_IP,
    MSG_STA_DISCONNECTED,
    MSG_PROV_SUCCESS,
    MSG_PROV_FAIL,
} msg_t;

#define STA_MAX_RETRY      6           // fast retries before declaring STA_FAILED
#define STA_REFAIL_MS      30000       // then a slow periodic re-attempt from failed
#define SOFTAP_NOCLIENT_MS 120000      // no client joined -> tear the AP down
#define SOFTAP_IDLE_MS     300000      // no HTTP traffic -> tear the AP down
#define PROV_TIMEOUT_MS    300000      // no successful pairing -> reboot to sync
#define TICK_MS            2000        // watchdog cadence

static net_state_t     s_state = NET_BOOT;
static QueueHandle_t   s_q;
static int             s_sta_retries;
static bool            s_softap_from_sync;  // remember the base mode to return to
static int64_t         s_prov_deadline_ms;  // provisioning timeout (ms uptime)
static int64_t         s_sta_refail_ms;     // next slow STA re-attempt (ms uptime)

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

net_state_t netmgr_state(void)     { return s_state; }
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
    case NET_PROVISIONING:   return "provisioning";
    default:                 return "?";
    }
}

static void post(msg_t m)
{
    // Bounded wait, never portMAX_DELAY: post() runs in WiFi/BLE event-task context,
    // and the manager task can be busy in a blocking transition (blesync_stop ~1 s).
    // Dropping a best-effort event is fine; the watchdog/periodic retry recover.
    if (s_q && xQueueSend(s_q, &m, pdMS_TO_TICKS(500)) != pdTRUE)
        ESP_LOGW(TAG, "queue full; dropped msg %d", (int)m);
}

void netmgr_request_softap(void)       { post(MSG_SOFTAP_START); }
void netmgr_request_stop_softap(void)  { post(MSG_SOFTAP_STOP); }
void netmgr_request_provisioning(void) { post(MSG_PROVISION); }
void netmgr_request_forget_wifi(void)  { post(MSG_FORGET); }

void netmgr_post_event(net_event_t ev)
{
    switch (ev) {
    case NETEV_STA_GOT_IP:       post(MSG_STA_GOT_IP); break;
    case NETEV_STA_DISCONNECTED: post(MSG_STA_DISCONNECTED); break;
    case NETEV_PROV_SUCCESS:     post(MSG_PROV_SUCCESS); break;
    case NETEV_PROV_FAIL:        post(MSG_PROV_FAIL); break;
    }
}

// --- transitions (all run on the manager task) ------------------------------

static void enter_sta(void)
{
    s_state = NET_STA_CONNECTING;
    s_sta_retries = 0;
    provisioning_set_connected(false);
    provisioning_sta_connect();                 // set STA mode + start + connect
    ESP_LOGI(TAG, "-> sta-connecting");
}

static void enter_sync(void)
{
    s_state = NET_SYNC_IDLE;
    provisioning_set_connected(false);
    blesync_start();                            // advertise for the phone
    ESP_LOGI(TAG, "-> sync-idle (BLE advertising)");
}

static void enter_provisioning(void)
{
    s_state = NET_PROVISIONING;
    s_prov_deadline_ms = now_ms() + PROV_TIMEOUT_MS;
    provisioning_prov_start();                  // BLE WiFi-provisioning manager
    ESP_LOGI(TAG, "-> provisioning (BLE)");
}

static void enter_softap(void)
{
    // Base mode we must restore when the session ends.
    s_softap_from_sync = (s_state == NET_SYNC_IDLE);

    if (s_softap_from_sync) {
        // blesync is up: bring the AP + server up first so the token exists, hand
        // the creds+token to the still-connected phone over BLE, then free BLE for
        // the transfer (BLE + SoftAP together starve the heap).
        if (!apmode_start_session() || !filesrv_start()) {
            ESP_LOGE(TAG, "softap start failed; back to sync");
            apmode_stop(); blesync_start(); s_state = NET_SYNC_IDLE; return;
        }
        blesync_notify_handoff();
        vTaskDelay(pdMS_TO_TICKS(1000));        // let the notify reach the phone
        blesync_stop();                         // returns ~48 KB to the heap
    } else {
        // STA base (or failed): drop the STA link, then raise the AP. No BLE here.
        provisioning_sta_disconnect();
        if (!apmode_start_session() || !filesrv_start()) {
            ESP_LOGE(TAG, "softap start failed; back to sta");
            apmode_stop(); enter_sta(); return;
        }
    }
    s_state = NET_SOFTAP;
    ESP_LOGI(TAG, "-> softap (base=%s)", s_softap_from_sync ? "sync" : "sta");
}

static void exit_softap(void)
{
    filesrv_stop();
    apmode_stop();
    if (s_softap_from_sync) enter_sync();
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

    case MSG_PROVISION:                          // one-way: reboot into provisioning
        ESP_LOGW(TAG, "provisioning requested -> reboot");
        provisioning_set_prov_pending(true);
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
        break;

    case MSG_FORGET:                             // erase creds + reboot into sync
        ESP_LOGW(TAG, "forget WiFi -> erase creds + reboot");
        provisioning_set_prov_pending(false);
        provisioning_forget();
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
        break;

    case MSG_STA_GOT_IP:
        if (s_state == NET_STA_CONNECTING || s_state == NET_STA_FAILED) {
            s_state = NET_STA_CONNECTED;
            s_sta_retries = 0;
            provisioning_set_connected(true);
            provisioning_start_sntp();
            ESP_LOGI(TAG, "-> sta-connected");
        }
        break;

    case MSG_STA_DISCONNECTED:
        // Only meaningful while we're trying to hold a STA link. In SOFTAP the
        // disconnect is us dropping STA on purpose -> ignore.
        if (s_state == NET_STA_CONNECTING || s_state == NET_STA_CONNECTED) {
            provisioning_set_connected(false);
            if (s_sta_retries++ < STA_MAX_RETRY) {
                s_state = NET_STA_CONNECTING;
                provisioning_sta_reconnect();
            } else {
                s_state = NET_STA_FAILED;
                s_sta_refail_ms = now_ms() + STA_REFAIL_MS;   // keep trying, slowly
                ESP_LOGW(TAG, "-> sta-failed (%d retries); slow retry in %d s",
                         s_sta_retries, STA_REFAIL_MS / 1000);
            }
        }
        break;

    case MSG_PROV_SUCCESS:                        // creds stored by the prov manager
        if (s_state == NET_PROVISIONING) {
            ESP_LOGI(TAG, "provisioning OK -> reboot to connect");
            provisioning_set_prov_pending(false);
            vTaskDelay(pdMS_TO_TICKS(1500));       // let the app see success first
            esp_restart();
        }
        break;

    case MSG_PROV_FAIL:
        ESP_LOGW(TAG, "provisioning attempt failed (retry in app)");
        break;                                    // stay advertising; user retries
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
    if (s_state == NET_PROVISIONING && now_ms() > s_prov_deadline_ms) {
        ESP_LOGW(TAG, "provisioning timed out -> reboot to sync");
        provisioning_set_prov_pending(false);
        esp_restart();
    }
    // A provisioned device that couldn't join (home WiFi briefly down) keeps trying
    // slowly instead of staying offline until a reboot.
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

    // Initial mode, decided once, at boot.
    if (provisioning_prov_pending())      enter_provisioning();
    else if (provisioning_has_creds())    enter_sta();
    else                                  enter_sync();

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
    xTaskCreate(netmgr_task, "netmgr", 4096, NULL, 6, NULL);
}
