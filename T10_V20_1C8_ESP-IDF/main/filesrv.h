// HTTP file server for device→phone sync (docs/DEVICE_FILE_SYNC.md §6). Serves
// the SD card over the current Wi-Fi interface (STA now; SoftAP later — the same
// server binds to whichever is up). SD must be mounted before starting.
#pragma once

#include <stdbool.h>

// Start the server (port 8080) + mDNS (t10.local) and mint a fresh session token.
// The token is logged to serial for curl testing; later it's delivered to the
// phone over the BLE handoff. Returns false on error.
bool filesrv_start(void);

void filesrv_stop(void);
bool filesrv_running(void);

// Current bearer token (32 hex chars), for the BLE WIFI_HANDOFF payload.
const char *filesrv_token(void);

#include <stdint.h>
int64_t filesrv_idle_ms(void);   // ms since the last HTTP request (0 if stopped)
