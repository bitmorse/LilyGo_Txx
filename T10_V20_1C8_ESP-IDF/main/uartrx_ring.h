// Pure "last N bytes received" ring-tail + hex formatter for the UART RX UI.
// Deliberately free of any ESP-IDF / FreeRTOS dependency so it is unit-tested on
// the host (see test/test_uartrx_ring.c); uartrx.c owns an instance and guards it.
#pragma once

#include <stdint.h>

#define UARTRX_LAST_KEEP 8            // bytes retained for the UI readout

typedef struct {
    uint8_t b[UARTRX_LAST_KEEP];
    int     len;                      // 0..UARTRX_LAST_KEEP
} uartrx_ring_t;

// Empty the buffer.
void uartrx_ring_reset(uartrx_ring_t *r);

// Record that `n` bytes arrived: keep only the most recent UARTRX_LAST_KEEP across
// calls (a small append after a full buffer drops the oldest). n <= 0 is a no-op.
void uartrx_ring_push(uartrx_ring_t *r, const uint8_t *buf, int n);

// Format the retained bytes as space-separated uppercase hex ("AA BB CC") into
// `out` (capacity `cap`, always NUL-terminated). Returns the string length. Never
// writes past `cap`; truncates to whole bytes if it doesn't fit.
int uartrx_ring_hex(const uartrx_ring_t *r, char *out, int cap);
