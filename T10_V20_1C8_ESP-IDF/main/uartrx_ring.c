#include "uartrx_ring.h"

#include <stdio.h>
#include <string.h>

void uartrx_ring_reset(uartrx_ring_t *r)
{
    r->len = 0;
}

void uartrx_ring_push(uartrx_ring_t *r, const uint8_t *buf, int n)
{
    if (n <= 0) return;

    if (n >= UARTRX_LAST_KEEP) {                 // new data alone fills/overflows
        memcpy(r->b, buf + (n - UARTRX_LAST_KEEP), UARTRX_LAST_KEEP);
        r->len = UARTRX_LAST_KEEP;
        return;
    }
    // Retain as many old bytes as still fit alongside the n new ones, keeping the
    // most recent of each. memmove: the retained tail and its destination overlap.
    int keep = UARTRX_LAST_KEEP - n;
    if (keep > r->len) keep = r->len;
    memmove(r->b, r->b + (r->len - keep), keep);
    memcpy(r->b + keep, buf, n);
    r->len = keep + n;
}

int uartrx_ring_hex(const uartrx_ring_t *r, char *out, int cap)
{
    if (cap <= 0) return 0;
    out[0] = '\0';
    int off = 0;
    for (int i = 0; i < r->len; i++) {
        int need = i ? 3 : 2;                     // " XX" after the first, else "XX"
        if (off + need >= cap) break;             // leave room for the NUL
        off += snprintf(out + off, cap - off, i ? " %02X" : "%02X", r->b[i]);
    }
    return off;
}
