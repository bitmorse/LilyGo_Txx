// WiFi credential provisioning over BLE (ESP-IDF wifi_provisioning + NimBLE).
// On first boot (no saved creds) the device advertises over BLE; the Espressif
// "ESP BLE Provisioning" app sends the home WiFi SSID + password, which are
// stored in NVS and used to connect automatically thereafter.
#pragma once

#include <stdbool.h>

typedef enum {
    PROV_IDLE,        // not started
    PROV_PAIRING,     // advertising over BLE, waiting for the app
    PROV_CONNECTING,  // credentials received, joining WiFi
    PROV_CONNECTED,   // WiFi connected (has IP)
    PROV_FAILED,      // wrong password / AP not found
} prov_state_t;

// Init NVS + WiFi + netif, then either connect (if already provisioned) or start
// BLE provisioning. Call once at startup (replaces wifi_scan_init()).
void provisioning_init(void);

prov_state_t provisioning_state(void);
const char  *provisioning_service_name(void);   // BLE device name shown in the app
const char  *provisioning_pop(void);            // proof-of-possession code
const char  *provisioning_qr_payload(void);     // JSON for the app's QR scanner

// Erase stored credentials and reboot into BLE pairing mode ("Setup WiFi" again).
void provisioning_reset_and_restart(void);
