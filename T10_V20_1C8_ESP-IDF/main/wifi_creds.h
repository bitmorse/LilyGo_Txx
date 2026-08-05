// Pure parser for the BLE WIFI_CREDS write payload used by provisioning-over-blesync
// (docs/DEVICE_STATE.md Stage 1). Format is NUL-separated: "<ssid>\0<pass>". The
// passphrase may be empty (open network); the SSID must be non-empty. No ESP deps ->
// host-tested (test/test_wifi_creds.c).
#pragma once

#include <stdbool.h>

// Parse `len` bytes at `buf` into NUL-terminated `ssid`/`pass`. Returns true on a
// valid payload that fits the caps (WiFi limits: SSID <= 32, pass <= 64, so caps of
// 33/65). Never writes past the caps; returns false on no separator, empty SSID, or
// an over-long field.
bool wifi_creds_parse(const unsigned char *buf, int len,
                      char *ssid, int ssid_cap, char *pass, int pass_cap);
