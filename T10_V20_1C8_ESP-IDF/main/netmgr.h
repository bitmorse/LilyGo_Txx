// Central connectivity state manager. Every module that used to poke esp_wifi /
// NimBLE directly posts a request here and the single manager task performs the
// transition. See CLAUDE.md "Connectivity vocabulary & rules (§)" for the model.
//
// The device RESTS on BLE (§5.1 sync-idle). Wi-Fi (STA) is joined only ON DEMAND --
// for a one-shot external-Wi-Fi file sync -- then dropped, returning to BLE (§2.3).
// BLE stays up while merely connected to Wi-Fi (§1.3); it is torn down only for the
// file transfer itself (SoftAP or WLAN serve, §3.2). Provisioning writes the blesync
// WIFI_CREDS characteristic; creds are stored + verified by a brief join (§4.3).
//
// State diagram (all non-BOOT paths return to SYNC_IDLE):
//
//   BOOT ─→ SYNC_IDLE  (always; never auto-joins Wi-Fi, §4.1)
//   SYNC_IDLE ──WIFI_CREDS──→ VERIFYING ──GOT_IP──→ (notify ok) ─→ SYNC_IDLE
//                                       └─fail/timeout─→ SYNC_IDLE (creds discarded)
//   SYNC_IDLE ──request_softap──┬─ hotspot pref ─→ SOFTAP ───────→ SYNC_IDLE
//                               └─ ext-wifi pref ─→ STA_CONNECTING ─GOT_IP→ WLAN_SERVE ─→ SYNC_IDLE
#pragma once

#include <stdbool.h>

typedef enum {
    NET_BOOT,
    NET_STA_CONNECTING,   // joining home Wi-Fi for an on-demand use (sync / feature)
    NET_STA_CONNECTED,    // on Wi-Fi + IP, BLE up -- held for a use, never a rest state
    NET_SYNC_IDLE,        // the resting state: blesync advertising, STA off (§5.1)
    NET_SOFTAP,           // hotspot file transfer in progress, BLE torn down (§5.2)
    NET_WLAN_SERVE,       // external-Wi-Fi file transfer in progress, BLE torn down (§5.3)
    NET_VERIFYING,        // candidate creds written over BLE; joining once to verify
} net_state_t;

// Launch the manager. Low-level init (provisioning_hw_init) must run first. The
// device always boots into SYNC_IDLE (BLE); Wi-Fi is joined on demand only.
void netmgr_start(void);

net_state_t netmgr_state(void);
const char *netmgr_state_str(net_state_t s);

// Full device-state snapshot JSON {state,provisioned,paired,mode,ip,dev} for the
// app (served on the BLE info READ and pushed on the status NOTIFY). Returns length.
int netmgr_status_json(char *out, int cap);

// --- requests (safe from any task: LVGL callback, HTTP handler, BLE callback) ---
void netmgr_request_softap(void);        // start a file-sync session (hotspot or ext-wifi)
void netmgr_request_stop_softap(void);   // end it -> return to BLE rest
void netmgr_request_provision(const char *ssid, const char *pass);  // store + verify creds
void netmgr_request_forget_wifi(void);   // erase creds -> sync-idle (live, no reboot)
void netmgr_request_set_mode(bool wlan); // pref: true=ext-wifi sync, false=hotspot sync

// --- internal events, posted by the WiFi/IP event handler in provisioning.c ---
typedef enum {
    NETEV_STA_GOT_IP,
    NETEV_STA_DISCONNECTED,
} net_event_t;
void netmgr_post_event(net_event_t ev);
