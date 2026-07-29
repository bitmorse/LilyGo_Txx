#pragma once

#include <stdint.h>

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
} ap_info_t;

// Bring up the WiFi stack in station mode (call once at startup).
void wifi_scan_init(void);

// Run a blocking scan. Fills up to `max` entries in `out`, returns the count.
int wifi_scan_run(ap_info_t *out, int max);
