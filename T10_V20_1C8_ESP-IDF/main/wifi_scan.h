#pragma once

#include <stdint.h>

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
} ap_info_t;

// Run a blocking scan. Fills up to `max` entries in `out`, returns the count.
// WiFi must already be started (provisioning_hw_init() does that).
int wifi_scan_run(ap_info_t *out, int max);
