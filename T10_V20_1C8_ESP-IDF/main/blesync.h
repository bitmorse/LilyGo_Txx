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
// Returns false if BLE could not start. Safe to call once.
bool blesync_start(void);

bool blesync_active(void);
