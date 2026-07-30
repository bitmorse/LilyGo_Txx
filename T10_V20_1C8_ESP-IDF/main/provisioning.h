// WiFi credential provisioning over BLE (ESP-IDF wifi_provisioning + NimBLE).
// On first boot (no saved creds) the device advertises over BLE; the Espressif
// "ESP BLE Provisioning" app sends the home WiFi SSID + password, which are
// stored in NVS and used to connect automatically thereafter.
#pragma once

#include <stdbool.h>
#include <stdint.h>

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

bool provisioning_is_connected(void);           // WiFi connected (has IP)
void provisioning_ssid(char *buf, int n);       // connected SSID (empty if none)
int  provisioning_rssi(void);                    // AP signal in dBm (0 if not connected)

// Erase stored credentials and reboot into BLE pairing mode ("Setup WiFi" again).
void provisioning_reset_and_restart(void);

// Wall-clock time, sourced from SNTP once WiFi is up (started on first GOT_IP).
bool     time_is_synced(void);   // true once NTP has set the RTC
uint64_t time_now_ns(void);      // UTC ns if synced, else monotonic esp_timer ns
