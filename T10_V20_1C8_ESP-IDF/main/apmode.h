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

// Enter SoftAP mode (leaves home Wi-Fi/STA). Fills ssid + a fresh passphrase.
// Returns false on error. Idempotent while active.
bool apmode_start(char *ssid, int ssid_cap, char *pass, int pass_cap);

// Leave SoftAP mode and reconnect to the provisioned home Wi-Fi (STA).
void apmode_stop(void);

bool apmode_active(void);
int  apmode_clients(void);      // number of stations currently associated
