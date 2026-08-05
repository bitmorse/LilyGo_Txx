#include "uartrx_rec.h"

#include <assert.h>
#include <stdio.h>

// Clamp an snprintf result to the actual written length (never past cap-1).
static int clamp(int n, int cap)
{
    if (n < 0) return 0;
    return n >= cap ? cap - 1 : n;
}

int uartrx_rec_state_json(char *out, int cap, const char *state,
                          unsigned bytes, long long elapsed_ms)
{
    if (cap <= 0) return 0;
    int n = snprintf(out, cap,
        "{\"state\":\"%s\",\"bytes\":%u,\"elapsed_ms\":%lld}",
        state, bytes, elapsed_ms);
    return clamp(n, cap);
}

int uartrx_rec_meta_json(char *out, int cap, const char *fw, const char *dev,
                         int baud, int gpio, bool time_synced)
{
    if (cap <= 0) return 0;
    int n = snprintf(out, cap,
        "{\"fw\":\"%s\",\"dev\":\"%s\",\"baud\":%d,\"gpio\":%d,\"time_synced\":%s}",
        fw, dev, baud, gpio, time_synced ? "true" : "false");
    return clamp(n, cap);
}

int uartrx_rec_uart_b64(char *out, int cap, const uint8_t *data, int n)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    assert(out && (data || n == 0) && n >= 0);

    // {"b64":"...="} -- 10 fixed chars + 4 base64 chars per 3 input bytes.
    int b64len = 4 * ((n + 2) / 3);
    int total  = 10 + b64len;                   // excludes the NUL
    if (cap <= total) return 0;                 // refuse rather than truncate

    char *p = out;
    *p++ = '{'; *p++ = '"'; *p++ = 'b'; *p++ = '6'; *p++ = '4'; *p++ = '"';
    *p++ = ':'; *p++ = '"';
    for (int i = 0; i < n; i += 3) {            // bounded by n
        uint32_t b0 = data[i];
        uint32_t b1 = (i + 1 < n) ? data[i + 1] : 0;
        uint32_t b2 = (i + 2 < n) ? data[i + 2] : 0;
        uint32_t w  = (b0 << 16) | (b1 << 8) | b2;
        *p++ = b64[(w >> 18) & 0x3F];
        *p++ = b64[(w >> 12) & 0x3F];
        *p++ = (i + 1 < n) ? b64[(w >> 6) & 0x3F] : '=';
        *p++ = (i + 2 < n) ? b64[w & 0x3F]        : '=';
    }
    *p++ = '"'; *p++ = '}';
    *p = '\0';
    assert((int)(p - out) == total);
    return total;
}
