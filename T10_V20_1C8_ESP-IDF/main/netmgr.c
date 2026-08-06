#include "netmgr.h"
#include "provisioning.h"
#include "blesync.h"
#include "apmode.h"
#include "filesrv.h"
#include "settings.h"
#include "viblog.h"
#include "uartrx.h"                // uartrx_is_recording() -- SD single-writer guard

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_log.h"

static const char *TAG = "netmgr";

// Requests + events funnel through one queue so every transition runs on the single
// manager task -- no locking, no re-entrancy, no two callers switching modes at once.
typedef enum {
    MSG_SOFTAP_START,
    MSG_SOFTAP_STOP,
    MSG_PROVISION_CREDS,   // app wrote WIFI_CREDS (creds in s_pend_ssid/s_pend_pass)
    MSG_FORGET,
    MSG_SET_MODE,          // switch pref_mode (s_pend_wlan)
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
static bool          s_pend_wlan;          // pref_mode requested via MSG_SET_MODE

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void get_sta_ip(char *out, int cap)
{
    snprintf(out, cap, "0.0.0.0");
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ipi;
    if (sta && esp_netif_get_ip_info(sta, &ipi) == ESP_OK && ipi.ip.addr)
        snprintf(out, cap, IPSTR, IP2STR(&ipi.ip));
}

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
    case NET_WLAN_SERVE:     return "wlan-serve";
    case NET_VERIFYING:      return "verifying";
    default:                 return "?";
    }
}

// Full device-state snapshot for the app (BLE info READ + status NOTIFY).
int netmgr_status_json(char *out, int cap)
{
    char ip[16];
    get_sta_ip(ip, sizeof(ip));
    return snprintf(out, cap,
        "{\"state\":\"%s\",\"provisioned\":%s,\"paired\":%s,\"mode\":\"%s\","
        "\"ip\":\"%s\",\"dev\":\"%s\"}",
        netmgr_state_str(s_state),
        provisioning_has_creds() ? "true" : "false",
        blesync_is_paired()      ? "true" : "false",
        settings_wlan_mode()     ? "wlan" : "ble",
        ip, provisioning_service_name());
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
void netmgr_request_set_mode(bool wlan) { s_pend_wlan = wlan; post(MSG_SET_MODE); }

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

    if (!apmode_start_session()) {             // SoftAP up (BLE still up; no httpd yet)
        ESP_LOGE(TAG, "softap start failed");
        apmode_stop();
        if (s_softap_from_sync) enter_sync(); else enter_sta();
        return;
    }
    // Order matters for heap: hand the phone the creds+token over BLE, tear BLE all
    // the way down (frees ~48 KB), and ONLY THEN start the heap-heavy HTTP server.
    // Starting httpd while BLE is still up leaves ~0 free -> flaky, no handoff.
    filesrv_new_token();                        // token for the handoff (no server yet)
    blesync_notify_handoff();
    vTaskDelay(pdMS_TO_TICKS(1000));            // let the notify reach the phone
    blesync_stop();
    if (!filesrv_start()) {                     // now with BLE's heap freed
        ESP_LOGE(TAG, "filesrv start failed (post-BLE-stop)");
        apmode_stop();
        if (s_softap_from_sync) enter_sync(); else enter_sta();
        return;
    }
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

// WLAN file serving: STA stays connected, the file server runs on the LAN, the phone
// pulls from the device's IP (no SoftAP join). BLE is still torn down for the
// transfer (Stage 0: BLE + httpd don't fit), and restored after.
static void enter_wlan_serve(void)
{
    char ip[16];
    get_sta_ip(ip, sizeof(ip));
    // Same heap ordering as enter_softap(): hand off over BLE, free BLE, THEN httpd.
    filesrv_new_token();                        // token for the handoff (no server yet)
    blesync_notify_wlan_handoff(ip, 8080, filesrv_token());
    vTaskDelay(pdMS_TO_TICKS(1000));            // let the notify reach the phone
    blesync_stop();                             // free BLE heap before starting httpd
    if (!filesrv_start()) {                     // now with BLE's heap freed
        ESP_LOGE(TAG, "wlan-serve: filesrv failed (post-BLE-stop)");
        enter_sta();                            // STA base: restart blesync + reconnect
        return;
    }
    s_state = NET_WLAN_SERVE;
    ESP_LOGI(TAG, "-> wlan-serve (%s:8080)", ip);
}

static void exit_wlan_serve(void)
{
    filesrv_stop();
    enter_sta();                                // STA still up; restart blesync + resync
}

// --- message handling -------------------------------------------------------

static void handle(msg_t m)
{
    switch (m) {
    case MSG_SOFTAP_START:
        // One SD writer at a time: don't raise a file server while the SD is being
        // written (vibration log OR the UART-RX MCAP recording). Two writers + the
        // file server on one no-PSRAM card corrupts data and starves the heap.
        if (viblog_is_running() || uartrx_is_recording()) {
            ESP_LOGW(TAG, "SD busy (recording) -> refusing sync");
            break;
        }
        if (s_state == NET_STA_CONNECTED) {
            enter_wlan_serve();                  // on home WiFi -> serve over the LAN
        } else if (s_state == NET_SYNC_IDLE || s_state == NET_STA_CONNECTING ||
                   s_state == NET_STA_FAILED) {
            enter_softap();                      // no LAN -> SoftAP
        }
        break;

    case MSG_SOFTAP_STOP:
        if      (s_state == NET_SOFTAP)     exit_softap();
        else if (s_state == NET_WLAN_SERVE) exit_wlan_serve();
        break;

    case MSG_SET_MODE:
        settings_set_wlan_mode(s_pend_wlan);
        if (provisioning_has_creds()) {
            if (s_pend_wlan && s_state == NET_SYNC_IDLE) {
                enter_sta();                     // BLE mode -> WLAN: join home WiFi
            } else if (!s_pend_wlan && (s_state == NET_STA_CONNECTING ||
                       s_state == NET_STA_CONNECTED || s_state == NET_STA_FAILED)) {
                provisioning_sta_disconnect();   // WLAN -> BLE: drop STA, BLE only
                enter_sync();
            }
        }
        blesync_notify_status();
        ESP_LOGI(TAG, "pref_mode := %s", s_pend_wlan ? "WLAN" : "BLE");
        break;

    case MSG_PROVISION_CREDS:
        if (s_state != NET_SOFTAP && s_state != NET_WLAN_SERVE) enter_verifying();
        break;

    case MSG_FORGET:                             // live: erase creds -> sync-idle (no reboot)
        ESP_LOGW(TAG, "forget WiFi -> sync-idle");
        settings_set_wlan_mode(false);           // no creds -> back to hotspot default
        provisioning_sta_disconnect();
        provisioning_forget();
        enter_sync();
        break;

    case MSG_STA_GOT_IP:
        if (s_state == NET_VERIFYING) {
            // Creds verified (already stored). Tell the phone. Hotspot is the default,
            // so DROP back to sync-idle (BLE) unless the user has explicitly enabled
            // External WiFi only -- adding/verifying WiFi is not a request to switch to it.
            blesync_notify_prov_result(true, NULL);
            provisioning_start_sntp();                   // grab time while briefly online
            if (settings_wlan_mode()) {
                s_state = NET_STA_CONNECTED;             // ext-WiFi mode -> stay connected
                s_sta_retries = 0;
                provisioning_set_connected(true);
                ESP_LOGI(TAG, "verified -> sta-connected (ext WiFi mode)");
            } else {
                provisioning_sta_disconnect();           // hotspot default -> back to BLE
                enter_sync();
                ESP_LOGI(TAG, "verified -> back to hotspot (BLE)");
            }
            blesync_notify_status();
        } else if (s_state == NET_STA_CONNECTING || s_state == NET_STA_FAILED) {
            s_state = NET_STA_CONNECTED;                 // user-requested ext WiFi connect
            s_sta_retries = 0;
            provisioning_set_connected(true);
            provisioning_start_sntp();
            blesync_notify_status();
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
    if (s_state == NET_WLAN_SERVE && filesrv_idle_ms() > SOFTAP_IDLE_MS) {
        ESP_LOGW(TAG, "wlan-serve idle -> teardown");
        exit_wlan_serve();
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
    // Provisioned + WLAN pref -> join home WiFi; provisioned + BLE pref (or no
    // creds) -> sync-idle (blesync up, STA off).
    bool creds = provisioning_has_creds(), wlan = settings_wlan_mode();
    ESP_LOGI(TAG, "boot: has_creds=%d wlan_mode=%d", creds, wlan);
    if (creds && wlan) enter_sta();
    else               enter_sync();

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
