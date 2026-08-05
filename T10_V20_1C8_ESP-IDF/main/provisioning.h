// Low-level WiFi + BLE-provisioning mechanism. The STATE (when to connect, when to
// provision, when to raise a SoftAP) is owned by netmgr.c -- this module only
// provides the primitives it drives, plus the wall-clock/time helpers and the
// getters the UI reads. The WiFi/IP/PROV event handler here just forwards events
// to netmgr_post_event(); it makes no policy decisions.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// One-time hardware bring-up: NVS, netif, event loop, esp_wifi_init, STA netif,
// event handlers, service name. Does NOT set a mode, start WiFi, or start BLE --
// netmgr decides that. Call once at boot, before netmgr_start().
void provisioning_hw_init(void);

// --- STA mechanism (called by netmgr) ---
bool provisioning_has_creds(void);      // stored home-WiFi SSID present
void provisioning_sta_connect(void);    // STA mode + start + connect (fresh attempt)
void provisioning_sta_reconnect(void);  // retry connect on the running STA
void provisioning_sta_disconnect(void); // drop the STA link (before raising SoftAP)
void provisioning_start_sntp(void);     // start SNTP once (on first GOT_IP)

// --- BLE WiFi-provisioning mechanism (called by netmgr; boot-only mode) ---
bool provisioning_prov_start(void);     // init + start wifi_prov_mgr over BLE; false on failure
void provisioning_prov_stop(void);      // stop + deinit the provisioning manager

// --- persistence ---
bool provisioning_prov_pending(void);          // "reboot into provisioning" flag
void provisioning_set_prov_pending(bool on);
void provisioning_forget(void);                // erase stored STA credentials

// --- state mirror for the UI (set by netmgr) ---
void provisioning_set_connected(bool on);
bool provisioning_is_connected(void);          // WiFi connected (has IP)
void provisioning_ssid(char *buf, int n);      // connected SSID (empty if none)
int  provisioning_rssi(void);                   // AP signal dBm (0 if not connected)
const char *provisioning_service_name(void);   // BLE name shown in the ESP prov app
const char *provisioning_pop(void);            // proof-of-possession code

// --- wall-clock time (SNTP once WiFi is up) ---
bool     time_is_synced(void);   // true once NTP has set the RTC
uint64_t time_now_ns(void);      // UTC ns if synced, else monotonic esp_timer ns
