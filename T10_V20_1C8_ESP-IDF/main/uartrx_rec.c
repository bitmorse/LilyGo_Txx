#include "uartrx_rec.h"

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
