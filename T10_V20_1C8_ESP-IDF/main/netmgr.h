// Central connectivity state manager: the single owner of the WiFi radio mode
// (STA vs SoftAP) and the BLE stack (blesync GATT vs WiFi-provisioning manager).
//
// Every module that used to poke esp_wifi / NimBLE directly now posts a request
// here and the manager task performs the transition, enforcing two hard invariants
// that this no-PSRAM ESP32 cannot violate without breaking:
//
//   * BLE and the SoftAP are NEVER up at once (together they starve the heap and
//     file transfers stall -- see blesync_stop()).
//   * Only one NimBLE user is ever active (blesync XOR wifi_prov_mgr). This is
//     guaranteed by deciding the top-level mode at boot: entering/leaving BLE
//     provisioning goes through a reboot, so a given boot runs exactly one.
//
// State diagram:
//
//   BOOT ─┬─ creds        ─→ STA_CONNECTING ─→ STA_CONNECTED ─→ (STA_FAILED)
//         ├─ no creds     ─→ SYNC_IDLE
//         └─ prov pending ─→ PROVISIONING ──(success)──→ reboot ─→ STA_CONNECTING
//
//   STA_CONNECTED / STA_FAILED / SYNC_IDLE ──request_softap──→ SOFTAP
//   SOFTAP ──stop/watchdog──→ back to the base mode it came from
#pragma once

#include <stdbool.h>

typedef enum {
    NET_BOOT,
    NET_STA_CONNECTING,   // joining home WiFi (provisioned)
    NET_STA_CONNECTED,    // home WiFi up + IP -> internet features + SNTP
    NET_STA_FAILED,       // couldn't join after retries
    NET_SYNC_IDLE,        // no creds: blesync advertising, radio idle
    NET_SOFTAP,           // File Sync SoftAP + HTTP server up, BLE torn down
    NET_PROVISIONING,     // BLE WiFi-provisioning manager active (boot-only)
} net_state_t;

// Launch the manager. Low-level init (provisioning_hw_init) must run first. The
// manager picks the initial state from the prov-pending flag / stored creds.
void netmgr_start(void);

net_state_t netmgr_state(void);
const char *netmgr_state_str(net_state_t s);

// True once STA is connected with an IP -- the gate for internet features
// (radio, ZRH traffic, SNTP). False in SoftAP / sync / provisioning states.
bool netmgr_internet_up(void);

// --- requests (safe from any task: LVGL callback, HTTP handler, BLE callback) ---
void netmgr_request_softap(void);        // start a File Sync SoftAP session
void netmgr_request_stop_softap(void);   // end it -> return to the base mode
void netmgr_request_provisioning(void);  // reboot into BLE WiFi provisioning
void netmgr_request_forget_wifi(void);   // erase creds + reboot (into sync mode)

// --- internal events, posted by the WiFi/IP/PROV event handler in provisioning.c ---
typedef enum {
    NETEV_STA_GOT_IP,
    NETEV_STA_DISCONNECTED,
    NETEV_PROV_SUCCESS,
    NETEV_PROV_FAIL,
} net_event_t;
void netmgr_post_event(net_event_t ev);
