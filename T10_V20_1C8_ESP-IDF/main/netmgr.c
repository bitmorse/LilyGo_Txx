#include "netmgr.h"
#include "provisioning.h"
#include "blesync.h"
#include "apmode.h"
#include "filesrv.h"
#include "settings.h"
#include "viblog.h"
#include "uartrx.h"                // uartrx_is_recording() -- SD single-writer guard
#include "radio.h"                 // radio_is_playing() -- heap guard for a sync

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
    MSG_SET_MODE,          // set the sync-transport preference (s_pend_wlan)
    MSG_STA_GOT_IP,
    MSG_STA_DISCONNECTED,
    MSG_WIFI_ACQUIRE,      // a feature wants Wi-Fi up (on-demand hold)
    MSG_WIFI_RELEASE,      // a feature is done with Wi-Fi
} msg_t;

#define STA_MAX_RETRY       6          // fast retries on a join drop before giving up
#define JOIN_TIMEOUT_MS     20000      // Wi-Fi join must complete within this, else abort
#define SOFTAP_NOCLIENT_MS  120000     // no client joined -> tear the AP down
#define SOFTAP_IDLE_MS      300000     // no HTTP traffic -> tear the transfer down
#define TICK_MS             2000       // watchdog cadence
#define BLE_NOTIFY_DRAIN_MS 1000       // let a BLE notify reach the phone before blesync_stop

static net_state_t   s_state = NET_BOOT;
static QueueHandle_t s_q;
static int           s_sta_retries;
static bool          s_sync_pending;       // a sync is waiting for Wi-Fi to come up (then serve)
static int           s_wifi_users;         // on-demand Wi-Fi hold refcount (features)
static int64_t       s_join_deadline;      // ms uptime by which a join must complete
static char          s_pend_ssid[33], s_pend_pass[65];   // creds awaiting verification
static bool          s_pend_wlan;          // pref requested via MSG_SET_MODE

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void get_sta_ip(char *out, int cap)
{
    snprintf(out, cap, "0.0.0.0");
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ipi;
    if (sta && esp_netif_get_ip_info(sta, &ipi) == ESP_OK && ipi.ip.addr)
        snprintf(out, cap, IPSTR, IP2STR(&ipi.ip));
}

net_state_t netmgr_state(void) { return s_state; }

const char *netmgr_state_str(net_state_t s)
{
    switch (s) {
    case NET_BOOT:           return "boot";
    case NET_STA_CONNECTING: return "sta-connecting";
    case NET_STA_CONNECTED:  return "sta-connected";
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

void netmgr_request_softap(void)        { post(MSG_SOFTAP_START); }
void netmgr_request_stop_softap(void)   { post(MSG_SOFTAP_STOP); }
void netmgr_request_forget_wifi(void)   { post(MSG_FORGET); }
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

// Called from a feature task: request Wi-Fi and block (up to timeout) until it's up.
// The refcount is maintained on the manager task; the caller just polls the connected
// flag. ALWAYS pair with netmgr_wifi_release(), even on a false return.
bool netmgr_wifi_hold(unsigned timeout_ms)
{
    post(MSG_WIFI_ACQUIRE);
    int64_t deadline = now_ms() + (int64_t)timeout_ms;
    while (now_ms() < deadline) {
        if (provisioning_is_connected()) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return provisioning_is_connected();
}

void netmgr_wifi_release(void) { post(MSG_WIFI_RELEASE); }

// --- transitions (all run on the manager task) ------------------------------
// CLAUDE.md §: the device RESTS on BLE (sync-idle, §5.1). Wi-Fi is joined only on
// demand -- for a one-shot external-Wi-Fi sync -- then dropped, back to BLE (§2.3).
// BLE stays up alongside STA (§1.3); it is torn down only for the file transfer (§3.2).

static void enter_sync(void)
{
    s_state = NET_SYNC_IDLE;
    s_sync_pending = false;
    provisioning_set_connected(false);
    blesync_start();
    ESP_LOGI(TAG, "-> sync-idle (BLE rest, §5.1)");
}

// Begin joining home Wi-Fi; `next` is the state to sit in while the join is in flight
// (NET_STA_CONNECTING for a sync, NET_VERIFYING for a creds check). BLE stays up (§1.3).
static void begin_join(net_state_t next)
{
    s_state = next;
    s_sta_retries = 0;
    s_join_deadline = now_ms() + JOIN_TIMEOUT_MS;
    provisioning_set_connected(false);
    blesync_start();
    provisioning_sta_connect();
    ESP_LOGI(TAG, "-> %s (joining Wi-Fi)", netmgr_state_str(next));
}

// App wrote WIFI_CREDS: store the candidate and join once to verify. GOT_IP -> notify
// ok then leave (back to BLE, §4.3); repeated failure/timeout discards the creds.
static void enter_verifying(void)
{
    if (!provisioning_store_creds(s_pend_ssid, s_pend_pass)) {
        blesync_notify_prov_result(false, "bad credentials");
        return;                                 // stay at rest
    }
    begin_join(NET_VERIFYING);
    ESP_LOGI(TAG, "verifying '%s'", s_pend_ssid);
}

static void enter_softap(void)
{
    if (!apmode_start_session()) {              // SoftAP up (BLE still up; no httpd yet)
        ESP_LOGE(TAG, "softap start failed");
        apmode_stop();
        enter_sync();
        return;
    }
    // Heap order (§1.2/§3.2): hand the phone the creds+token over BLE, tear BLE all the
    // way down (frees ~48 KB), and ONLY THEN start the heap-heavy HTTP server.
    filesrv_new_token();
    blesync_notify_handoff();
    vTaskDelay(pdMS_TO_TICKS(BLE_NOTIFY_DRAIN_MS));
    blesync_stop();
    if (!filesrv_start()) {
        ESP_LOGE(TAG, "filesrv start failed (post-BLE-stop)");
        apmode_stop();
        enter_sync();
        return;
    }
    s_state = NET_SOFTAP;
    ESP_LOGI(TAG, "-> softap (§5.2)");
}

static void exit_softap(void)
{
    filesrv_stop();
    apmode_stop();
    enter_sync();                               // hotspot is ephemeral -> back to BLE
}

// External-Wi-Fi transfer: already joined to home Wi-Fi (from begin_join), serve the
// file server on the LAN. BLE torn down for the transfer (§3.2); exit drops Wi-Fi (§2.3).
static void enter_wlan_serve(void)
{
    char ip[16];
    get_sta_ip(ip, sizeof(ip));
    filesrv_new_token();
    blesync_notify_wlan_handoff(ip, 8080, filesrv_token());
    vTaskDelay(pdMS_TO_TICKS(BLE_NOTIFY_DRAIN_MS));
    blesync_stop();
    if (!filesrv_start()) {
        ESP_LOGE(TAG, "wlan-serve: filesrv failed (post-BLE-stop)");
        provisioning_sta_disconnect();
        enter_sync();
        return;
    }
    s_state = NET_WLAN_SERVE;
    ESP_LOGI(TAG, "-> wlan-serve (%s:8080, §5.3)", ip);
}

static void exit_wlan_serve(void)
{
    filesrv_stop();
    if (s_wifi_users > 0) {                      // a feature still holds Wi-Fi -> keep STA
        s_state = NET_STA_CONNECTED;
        blesync_start();                         // bring BLE back (§3.2 restore)
        provisioning_set_connected(true);
        ESP_LOGI(TAG, "wlan-serve done; Wi-Fi held by a feature -> sta-connected");
    } else {
        provisioning_sta_disconnect();           // ephemeral: leave Wi-Fi (§2.3)
        enter_sync();
    }
}

// Abort an in-flight join (fail or timeout) back to BLE rest. For a verify, tell the
// phone + discard the creds; for a sync join, the app just times out on the handoff.
static void abort_join(const char *why)
{
    bool verify = (s_state == NET_VERIFYING);
    ESP_LOGW(TAG, "Wi-Fi join %s -> back to BLE", why);
    if (verify) {
        provisioning_forget();
        blesync_notify_prov_result(false, "wrong password or AP not found");
    }
    s_sync_pending = false;
    provisioning_sta_disconnect();
    enter_sync();
}

// --- message handling -------------------------------------------------------

static void handle(msg_t m)
{
    switch (m) {
    case MSG_SOFTAP_START:
        // One SD writer at a time (recording), and don't start the heap-heavy file
        // server while the radio stream is eating RAM -- both would OOM (§1.1).
        if (viblog_is_running() || uartrx_is_recording()) {
            ESP_LOGW(TAG, "SD busy (recording) -> refusing sync");
            break;
        }
        if (radio_is_playing()) {
            ESP_LOGW(TAG, "radio playing (heap) -> refusing sync");
            break;
        }
        if (s_state == NET_STA_CONNECTED && settings_wlan_mode()) {
            enter_wlan_serve();                 // already on Wi-Fi (a feature holds it)
            break;
        }
        if (s_state != NET_SYNC_IDLE) break;    // else only start a sync from BLE rest
        if (settings_wlan_mode() && provisioning_has_creds()) {
            s_sync_pending = true;              // serve once Wi-Fi is up (ephemeral, §2.3)
            begin_join(NET_STA_CONNECTING);
        } else {
            enter_softap();                     // hotspot (default, §2.2)
        }
        break;

    case MSG_SOFTAP_STOP:
        if      (s_state == NET_SOFTAP)     exit_softap();
        else if (s_state == NET_WLAN_SERVE) exit_wlan_serve();
        break;

    case MSG_SET_MODE:
        // Preference only: which transport the NEXT sync uses. No live switch -- the
        // device always rests on BLE (§4.1/§4.2); Wi-Fi is joined on demand.
        settings_set_wlan_mode(s_pend_wlan);
        blesync_notify_status();
        ESP_LOGI(TAG, "sync pref := %s", s_pend_wlan ? "ext-wifi" : "hotspot");
        break;

    case MSG_PROVISION_CREDS:
        if (s_state == NET_SYNC_IDLE) enter_verifying();   // only verify from rest
        break;

    case MSG_FORGET:                            // live: erase creds -> sync-idle (no reboot)
        ESP_LOGW(TAG, "forget Wi-Fi -> sync-idle");
        settings_set_wlan_mode(false);          // no creds -> back to hotspot default
        provisioning_sta_disconnect();
        provisioning_forget();
        enter_sync();
        break;

    case MSG_STA_GOT_IP:
        if (s_state == NET_VERIFYING) {
            blesync_notify_prov_result(true, NULL);
            provisioning_start_sntp();          // grab time while briefly online
            provisioning_sta_disconnect();      // ephemeral: leave after verify (§4.3)
            enter_sync();
            blesync_notify_status();
            ESP_LOGI(TAG, "verified -> back to BLE");
        } else if (s_state == NET_STA_CONNECTING) {
            provisioning_start_sntp();
            if (s_sync_pending) {
                s_sync_pending = false;
                enter_wlan_serve();             // joined -> serve the sync
            } else {
                s_state = NET_STA_CONNECTED;    // held for a feature (Stage B)
                provisioning_set_connected(true);
                blesync_notify_status();
                ESP_LOGI(TAG, "-> sta-connected");
            }
        }
        break;

    case MSG_STA_DISCONNECTED:
        if (s_state == NET_VERIFYING || s_state == NET_STA_CONNECTING) {
            if (s_sta_retries++ < STA_MAX_RETRY) provisioning_sta_reconnect();
            else                                 abort_join("failed");
        } else if (s_state == NET_STA_CONNECTED) {
            provisioning_set_connected(false);
            if (s_wifi_users > 0) {              // a feature still wants Wi-Fi -> restore
                s_state = NET_STA_CONNECTING;
                s_sta_retries = 0;
                s_join_deadline = now_ms() + JOIN_TIMEOUT_MS;
                provisioning_sta_reconnect();
            } else {
                enter_sync();                   // no holders (race) -> rest
            }
        }
        break;

    case MSG_WIFI_ACQUIRE:
        s_wifi_users++;
        // Join only from BLE rest; if a sync/join/hold is already in flight, this just
        // adds a reference and rides the existing connection.
        if (s_state == NET_SYNC_IDLE && provisioning_has_creds())
            begin_join(NET_STA_CONNECTING);     // s_sync_pending stays false -> feature hold
        else if (s_state == NET_SYNC_IDLE)
            ESP_LOGW(TAG, "wifi hold requested but no creds");
        break;

    case MSG_WIFI_RELEASE:
        if (s_wifi_users > 0) s_wifi_users--;
        if (s_wifi_users == 0 &&
            (s_state == NET_STA_CONNECTED || s_state == NET_STA_CONNECTING)) {
            provisioning_sta_disconnect();
            enter_sync();                       // last holder gone -> back to BLE (§2.3)
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
    // Bounded join: never hang in a joining state if Wi-Fi never comes up.
    if ((s_state == NET_STA_CONNECTING || s_state == NET_VERIFYING) &&
        now_ms() > s_join_deadline) {
        abort_join("timed out");
    }
}

static void netmgr_task(void *arg)
{
    (void)arg;

    // The device always boots into BLE rest -- it never auto-joins Wi-Fi, even with
    // stored creds (§4.1). Wi-Fi is joined on demand (a sync, or a feature in Stage B).
    ESP_LOGI(TAG, "boot: has_creds=%d sync_pref=%s", provisioning_has_creds(),
             settings_wlan_mode() ? "ext-wifi" : "hotspot");
    enter_sync();

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
