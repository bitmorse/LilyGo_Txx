// Unit tests for the AccelBatch protobuf encoder (main/accel_encode.c).
#include "test_framework.h"
#include "accel_encode.h"

#include <string.h>
#include <math.h>

// --- varint --------------------------------------------------------------

static void test_varint_small(void)
{
    uint8_t b[8];
    CHECK_EQ(accel_enc_varint(b, 0), 1);   CHECK_EQ(b[0], 0x00);
    CHECK_EQ(accel_enc_varint(b, 1), 1);   CHECK_EQ(b[0], 0x01);
    CHECK_EQ(accel_enc_varint(b, 127), 1); CHECK_EQ(b[0], 0x7F);
}

static void test_varint_multibyte(void)
{
    uint8_t b[8];
    CHECK_EQ(accel_enc_varint(b, 128), 2);
    CHECK_EQ(b[0], 0x80); CHECK_EQ(b[1], 0x01);
    CHECK_EQ(accel_enc_varint(b, 300), 2);
    CHECK_EQ(b[0], 0xAC); CHECK_EQ(b[1], 0x02);
    CHECK_EQ(accel_enc_varint(b, 2000), 2);     // 500 samples * 4 bytes
    CHECK_EQ(b[0], 0xD0); CHECK_EQ(b[1], 0x0F);
    CHECK_EQ(accel_enc_varint(b, 0xFFFFFFFFu), 5);
}

// --- batch encoding ------------------------------------------------------

static void test_encode_layout(void)
{
    float x[2] = { 1.0f, 2.0f }, y[2] = { -1.0f, 0.5f }, z[2] = { 9.81f, 0.0f };
    uint8_t out[256];
    uint64_t t0 = 0x0102030405060708ULL;
    uint32_t len = accel_encode_batch(out, sizeof(out), t0, 4000, x, y, z, 2);

    CHECK(len > 0);
    // field 1: t0_ns, fixed64 -> tag 0x09 then 8 LE bytes
    CHECK_EQ(out[0], 0x09);
    CHECK_EQ(out[1], 0x08); CHECK_EQ(out[8], 0x01);
    // field 2: rate_hz, varint -> tag 0x10, 4000 = 0xA0 0x1F
    CHECK_EQ(out[9], 0x10);
    CHECK_EQ(out[10], 0xA0); CHECK_EQ(out[11], 0x1F);
    // field 3: x packed floats -> tag 0x1A, len 8 (2 floats)
    CHECK_EQ(out[12], 0x1A);
    CHECK_EQ(out[13], 0x08);
    float fx;
    memcpy(&fx, &out[14], 4); CHECK(fx == 1.0f);
    memcpy(&fx, &out[18], 4); CHECK(fx == 2.0f);
    // field 4 tag follows the 8 float bytes
    CHECK_EQ(out[22], 0x22);
    // field 5 tag
    CHECK_EQ(out[22 + 1 + 1 + 8], 0x2A);
}

static void test_encode_length_matches_bound(void)
{
    float buf[500];
    for (int i = 0; i < 500; i++) buf[i] = (float)i;
    uint8_t out[ACCEL_MSG_MAXLEN(500)];
    uint32_t len = accel_encode_batch(out, sizeof(out), 1, 4000,
                                      buf, buf, buf, 500);
    CHECK(len > 0);
    CHECK(len <= ACCEL_MSG_MAXLEN(500));   // never exceeds the documented bound
    // exact: 9 (t0) + 3 (rate) + 3*(1+2+2000) = 9+3+6009 = 6021
    CHECK_EQ(len, 6021);
}

static void test_encode_rejects_small_buffer(void)
{
    float x[2] = {0}, y[2] = {0}, z[2] = {0};
    uint8_t out[8];                         // way too small
    CHECK_EQ(accel_encode_batch(out, sizeof(out), 0, 4000, x, y, z, 2), 0);
}

static void test_encode_zero_samples(void)
{
    uint8_t out[64];
    uint32_t len = accel_encode_batch(out, sizeof(out), 42, 4000,
                                      NULL, NULL, NULL, 0);
    CHECK(len > 0);                          // header fields still present
    CHECK_EQ(out[0], 0x09);
}

int main(void)
{
    printf("test_accel_encode\n");
    RUN(test_varint_small);
    RUN(test_varint_multibyte);
    RUN(test_encode_layout);
    RUN(test_encode_length_matches_bound);
    RUN(test_encode_rejects_small_buffer);
    RUN(test_encode_zero_samples);
    return REPORT();
}
