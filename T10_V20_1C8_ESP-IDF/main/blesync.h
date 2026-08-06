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
#include <stdint.h>

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

// WLAN-path handoff (STA stays up; phone pulls from the device's LAN IP).
void blesync_notify_wlan_handoff(const char *ip, int port, const char *token);

// Notify the phone of the provisioning result of its last WIFI_CREDS write (netmgr
// calls this after verification succeeds/fails).
void blesync_notify_prov_result(bool ok, const char *err);

// Tell the phone a START_SYNC was refused because the device is busy ("recording"|"radio").
void blesync_notify_sync_busy(const char *reason);

// Push the full device-state snapshot to the connected phone (netmgr calls this on
// state transitions).
void blesync_notify_status(void);

bool blesync_active(void);

// True if a phone is BLE-bonded ("device added"). Only meaningful while BLE is up.
bool blesync_is_paired(void);

// The 6-digit passkey to show on the TFT during pairing, or 0 if none is pending.
// The UI polls this and displays it (io_cap = DISPLAY_ONLY MITM pairing).
uint32_t blesync_passkey(void);
