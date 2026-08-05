// Central connectivity state manager. Every module that used to poke esp_wifi /
// NimBLE directly posts a request here and the single manager task performs the
// transition. See docs/DEVICE_STATE.md for the full design.
//
// blesync (the BLE control channel) is up in EVERY state except NET_SOFTAP: Stage 0
// measured that BLE + STA coexist (~50 KB free) but BLE + the HTTP file server do
// not, so file serving (SoftAP) tears BLE down for the transfer and restores it.
// WiFi provisioning is done by writing the blesync WIFI_CREDS characteristic (no
// wifi_prov_mgr, no reboots): creds are stored as a candidate and only committed on
// a confirmed GOT_IP (NET_VERIFYING).
//
// State diagram:
//
//   BOOT ─┬─ creds     ─→ STA_CONNECTING ─→ STA_CONNECTED ─→ (STA_FAILED ⟳ retry)
//         └─ no creds  ─→ SYNC_IDLE
//   SYNC_IDLE ──WIFI_CREDS──→ VERIFYING ──GOT_IP──→ STA_CONNECTED
//                                       └─fail──→ SYNC_IDLE (creds discarded)
//   STA_* / SYNC_IDLE ──request_softap──→ SOFTAP ──stop/watchdog──→ back to base
#pragma once

#include <stdbool.h>

typedef enum {
    NET_BOOT,
    NET_STA_CONNECTING,   // joining home WiFi (provisioned)
    NET_STA_CONNECTED,    // home WiFi up + IP -> internet features + SNTP
    NET_STA_FAILED,       // couldn't join after retries
    NET_SYNC_IDLE,        // no creds, or BLE pref: blesync advertising, STA off
    NET_SOFTAP,           // File Sync SoftAP + HTTP server up, BLE torn down
    NET_WLAN_SERVE,       // STA stays up; file server on the LAN, BLE torn down
    NET_VERIFYING,        // candidate creds written over BLE; trying to join
} net_state_t;

// Launch the manager. Low-level init (provisioning_hw_init) must run first. The
// manager picks the initial state from the stored creds (no reboot branch).
void netmgr_start(void);

net_state_t netmgr_state(void);
const char *netmgr_state_str(net_state_t s);

// Full device-state snapshot JSON {state,provisioned,paired,mode,ip,dev} for the
// app (served on the BLE info READ and pushed on the status NOTIFY). Returns length.
int netmgr_status_json(char *out, int cap);

// True once STA is connected with an IP -- the gate for internet features
// (radio, ZRH traffic, SNTP). False in SoftAP / sync / verifying states.
bool netmgr_internet_up(void);

// --- requests (safe from any task: LVGL callback, HTTP handler, BLE callback) ---
void netmgr_request_softap(void);        // start a file-sync session (SoftAP or WLAN)
void netmgr_request_stop_softap(void);   // end it -> return to the base mode
void netmgr_request_provision(const char *ssid, const char *pass);  // store + verify creds
void netmgr_request_forget_wifi(void);   // erase creds -> sync-idle (live, no reboot)
void netmgr_request_set_mode(bool wlan); // pref_mode: true=WLAN (join WiFi), false=BLE

// --- internal events, posted by the WiFi/IP event handler in provisioning.c ---
typedef enum {
    NETEV_STA_GOT_IP,
    NETEV_STA_DISCONNECTED,
} net_event_t;
void netmgr_post_event(net_event_t ev);
