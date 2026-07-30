// Protobuf encoder for the vibration-log AccelBatch message. Split out from
// viblog.c so it is host-compilable and unit-testable (no FreeRTOS/ESP deps).
//
//   message AccelBatch {            // proto3, package vibration
//     fixed64 t0_ns   = 1;          // UTC ns of the first sample in the batch
//     uint32  rate_hz = 2;
//     repeated float x = 3;         // packed; raw little-endian IEEE-754
//     repeated float y = 4;
//     repeated float z = 5;
//   }
#pragma once

#include <stdint.h>
#include <stddef.h>

// Upper bound on the encoded size of an n-sample batch, for buffer sizing:
//   9 (t0) + 6 (rate, worst case) + 3*(1 + 5 + n*4)  <=  33 + 12*n
#define ACCEL_MSG_MAXLEN(n) ((size_t)(33 + (size_t)(n) * 12))

// Encode a base-128 varint. Returns the number of bytes written (1..5).
int accel_enc_varint(uint8_t *p, uint32_t v);

// Encode one AccelBatch into `out` (capacity `cap`). x/y/z are n-element arrays
// of acceleration in g (may be NULL iff n == 0). Returns the encoded length, or
// 0 on bad arguments or if `cap` is too small (never writes past `cap`).
uint32_t accel_encode_batch(uint8_t *out, size_t cap, uint64_t t0_ns,
                            uint32_t rate_hz, const float *x, const float *y,
                            const float *z, int n);
