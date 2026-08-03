// SoftAP "field sync" mode: the device becomes its own Wi-Fi access point so a
// phone/laptop joins it DIRECTLY (no router, no home network) and pulls files
// over the HTTP server at http://192.168.4.1:8080. This is the primary transport
// for the field/unprovisioned case (docs/DEVICE_FILE_SYNC.md).
//
// The SSID is static & deterministic (Octanis-XXXX, last two MAC bytes) as
// AccessorySetupKit requires; the WPA2 passphrase is random per session and is
// meant to reach the phone over the (encrypted) BLE handoff — for now it is also
// shown on screen / logged for manual testing.
#pragma once

#include <stdbool.h>

// Enter SoftAP mode (leaves STA). SSID + passphrase are computed internally and
// exposed via apmode_ssid()/apmode_pass(). Returns false on error. Idempotent
// while active. Driven by netmgr, which owns the WiFi mode and BLE lifecycle.
bool apmode_start_session(void);

// Tear the SoftAP down and switch back to STA mode. Does NOT reconnect -- netmgr
// decides whether to rejoin home WiFi or stay in sync-idle.
void apmode_stop(void);

const char *apmode_ssid(void);  // current SoftAP SSID  ("" if not active)
const char *apmode_pass(void);  // current SoftAP passphrase ("" if not active)

bool apmode_active(void);
int  apmode_clients(void);      // number of stations currently associated

#include <stdint.h>
int64_t apmode_no_client_ms(void);   // ms with zero clients (0 if a client is on)
