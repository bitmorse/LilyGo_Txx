// BLE control channel for phone file sync (docs/DEVICE_FILE_SYNC.md §3).
//
// A NimBLE GATT peripheral the phone app connects to. It exposes device info,
// takes commands (notably "start SoftAP"), and hands the SoftAP SSID/passphrase +
// server token back over a notify characteristic -- the "magic" handoff so the
// phone can silently join the device's AP and pull files over HTTP.
//
// This owns the NimBLE stack, so it is mutually exclusive with the WiFi
// provisioning manager (which also uses NimBLE). Start it in "sync mode".
#pragma once

#include <stdbool.h>

// Init NimBLE + the sync GATT service and start advertising as "Octanis-XXXX".
// Returns false if BLE could not start. Re-callable after blesync_stop().
bool blesync_start(void);

// Tear the BLE stack fully down (host + controller), returning its RAM to the heap.
// netmgr calls this once the Wi-Fi handoff is delivered, because BLE + SoftAP +
// LWIP together exhaust the heap on this no-PSRAM ESP32.
void blesync_stop(void);

// Send the SoftAP creds + server token to the connected phone over the status
// notify characteristic. netmgr calls this after the AP + file server are up and
// before blesync_stop().
void blesync_notify_handoff(void);

bool blesync_active(void);
