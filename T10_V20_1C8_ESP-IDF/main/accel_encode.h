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

// Upper bounds on the encoded size of an n-sample batch, for buffer sizing.
// Header <= 15 bytes (t0 fixed64 + worst-case rate varint), each packed-float
// field <= 6 + n*4 bytes; AccelBatch has 3 fields, ImuAux has 7.
#define ACCEL_MSG_MAXLEN(n)  ((size_t)(15 + 3 * (6 + (size_t)(n) * 4)))
#define IMUAUX_MSG_MAXLEN(n) ((size_t)(15 + 7 * (6 + (size_t)(n) * 4)))

// Encode a base-128 varint. Returns the number of bytes written (1..5).
int accel_enc_varint(uint8_t *p, uint32_t v);

// Encode one AccelBatch into `out` (capacity `cap`). x/y/z are n-element arrays
// of acceleration in g (may be NULL iff n == 0). Returns the encoded length, or
// 0 on bad arguments or if `cap` is too small (never writes past `cap`).
uint32_t accel_encode_batch(uint8_t *out, size_t cap, uint64_t t0_ns,
                            uint32_t rate_hz, const float *x, const float *y,
                            const float *z, int n);

// Encode one ImuAux batch (gyro deg/s, mag uT, temp degC), same convention as
// accel_encode_batch. All seven arrays are n-element (may be NULL iff n == 0).
uint32_t imuaux_encode_batch(uint8_t *out, size_t cap, uint64_t t0_ns,
                             uint32_t rate_hz, const float *gx, const float *gy,
                             const float *gz, const float *mx, const float *my,
                             const float *mz, const float *temp_c, int n);
