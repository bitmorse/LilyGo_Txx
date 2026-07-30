#include "accel_encode.h"

#include <assert.h>
#include <string.h>

// Max samples we ever encode in one batch (guards the n*4 size math; the actual
// batch size is 500, well under this). Keeps rule "all bounds are explicit".
#define MAX_SAMPLES 100000

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

// Packed float field: tag, length varint, then raw LE float bytes (ESP32 and the
// x86/arm test host are both little-endian IEEE-754 -> the memory image is the
// protobuf packed-float wire form).
static uint8_t *enc_floats(uint8_t *p, uint8_t tag, const float *a, int n)
{
    *p++ = tag;
    p += accel_enc_varint(p, (uint32_t)(n * 4));
    if (n > 0) {
        assert(a != NULL);
        memcpy(p, a, (size_t)n * 4);
    }
    return p + (size_t)n * 4;
}

uint32_t accel_encode_batch(uint8_t *out, size_t cap, uint64_t t0_ns,
                            uint32_t rate_hz, const float *x, const float *y,
                            const float *z, int n)
{
    assert(out != NULL);
    if (n < 0 || n > MAX_SAMPLES) return 0;
    if (n > 0 && (x == NULL || y == NULL || z == NULL)) return 0;

    // Compute the exact encoded length up front, then only write if it fits.
    size_t arr   = (size_t)n * 4;
    size_t field = 1 + (size_t)varint_len((uint32_t)arr) + arr;   // one x/y/z
    size_t need  = 9                                              // t0 fixed64
                 + 1 + (size_t)varint_len(rate_hz)                // rate
                 + 3 * field;                                     // x, y, z
    if (need > cap) return 0;

    uint8_t *p = out;
    *p++ = 0x09;                              // field 1, wire type 1 (fixed64)
    memcpy(p, &t0_ns, 8); p += 8;
    *p++ = 0x10;                              // field 2, wire type 0 (varint)
    p += accel_enc_varint(p, rate_hz);
    p = enc_floats(p, 0x1A, x, n);            // field 3
    p = enc_floats(p, 0x22, y, n);            // field 4
    p = enc_floats(p, 0x2A, z, n);            // field 5

    assert((size_t)(p - out) == need);
    return (uint32_t)(p - out);
}
