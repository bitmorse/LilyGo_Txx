#include "accel_encode.h"

#include <assert.h>
#include <string.h>

// Max samples we ever encode in one batch (guards the n*4 size math; the actual
// batch sizes are 500 / 50, well under this). Keeps all bounds explicit.
#define MAX_SAMPLES 100000
#define MAX_FIELDS  8            // AccelBatch has 3 float fields, ImuAux has 7

int accel_enc_varint(uint8_t *p, uint32_t v)
{
    assert(p != NULL);
    int i = 0;
    while (v >= 0x80) {          // bounded: at most 5 iterations for uint32
        p[i++] = (uint8_t)(v & 0x7F) | 0x80;
        v >>= 7;
    }
    p[i++] = (uint8_t)v;
    return i;
}

static int varint_len(uint32_t v)
{
    int n = 1;
    while (v >= 0x80) { v >>= 7; n++; }
    return n;
}

// Encode t0_ns (field 1, fixed64) + rate_hz (field 2, varint) + `nf` packed-float
// fields numbered 3..(2+nf) from arrays[]. Each field's wire form is: tag, length
// varint, then the raw LE float bytes (ESP32 and the x86/arm test host are both
// little-endian IEEE-754, so the memory image *is* the packed-float wire form).
// Returns the encoded length, or 0 on bad args / if it wouldn't fit in `cap`.
static uint32_t encode_common(uint8_t *out, size_t cap, uint64_t t0, uint32_t rate,
                              const float *const *arrays, int nf, int n)
{
    assert(out != NULL);
    assert(nf >= 1 && nf <= MAX_FIELDS);
    if (n < 0 || n > MAX_SAMPLES) return 0;
    if (n > 0)
        for (int f = 0; f < nf; f++)
            if (arrays[f] == NULL) return 0;

    // Field numbers are <= 9, so every tag is a single byte.
    size_t arr   = (size_t)n * 4;
    size_t field = 1 + (size_t)varint_len((uint32_t)arr) + arr;   // one float array
    size_t need  = 9                                              // t0 fixed64
                 + 1 + (size_t)varint_len(rate)                   // rate
                 + (size_t)nf * field;
    if (need > cap) return 0;

    uint8_t *p = out;
    *p++ = 0x09;                                    // field 1, wire type 1
    memcpy(p, &t0, 8); p += 8;
    *p++ = 0x10;                                    // field 2, wire type 0
    p += accel_enc_varint(p, rate);
    for (int f = 0; f < nf; f++) {
        *p++ = (uint8_t)(((3 + f) << 3) | 2);       // field (3+f), wire type 2
        p += accel_enc_varint(p, (uint32_t)arr);
        if (n > 0) memcpy(p, arrays[f], arr);
        p += arr;
    }

    assert((size_t)(p - out) == need);
    return (uint32_t)(p - out);
}

uint32_t accel_encode_batch(uint8_t *out, size_t cap, uint64_t t0_ns,
                            uint32_t rate_hz, const float *x, const float *y,
                            const float *z, int n)
{
    const float *arrays[3] = { x, y, z };
    return encode_common(out, cap, t0_ns, rate_hz, arrays, 3, n);
}

uint32_t imuaux_encode_batch(uint8_t *out, size_t cap, uint64_t t0_ns,
                             uint32_t rate_hz, const float *gx, const float *gy,
                             const float *gz, const float *mx, const float *my,
                             const float *mz, const float *temp_c, int n)
{
    const float *arrays[7] = { gx, gy, gz, mx, my, mz, temp_c };
    return encode_common(out, cap, t0_ns, rate_hz, arrays, 7, n);
}
