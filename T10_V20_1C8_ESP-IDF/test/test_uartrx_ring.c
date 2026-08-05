// Unit tests for the UART RX "last N bytes" ring-tail + hex formatter
// (main/uartrx_ring.c). Pure host tests -- no ESP-IDF.
#include "test_framework.h"
#include "uartrx_ring.h"

#include <string.h>

static void test_empty(void)
{
    uartrx_ring_t r;
    uartrx_ring_reset(&r);
    CHECK_EQ(r.len, 0);
    char out[8];
    CHECK_EQ(uartrx_ring_hex(&r, out, sizeof(out)), 0);
    CHECK(strcmp(out, "") == 0);
}

static void test_push_small(void)
{
    uartrx_ring_t r;
    uartrx_ring_reset(&r);
    uint8_t d[] = {0xAA, 0xBB};
    uartrx_ring_push(&r, d, 2);
    CHECK_EQ(r.len, 2);
    CHECK_EQ(r.b[0], 0xAA);
    CHECK_EQ(r.b[1], 0xBB);
    char out[16];
    uartrx_ring_hex(&r, out, sizeof(out));
    CHECK(strcmp(out, "AA BB") == 0);
}

static void test_fill_exact(void)
{
    uartrx_ring_t r;
    uartrx_ring_reset(&r);
    uint8_t d[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uartrx_ring_push(&r, d, 8);
    CHECK_EQ(r.len, 8);
    CHECK_EQ(r.b[0], 1);
    CHECK_EQ(r.b[7], 8);
    char out[32];
    uartrx_ring_hex(&r, out, sizeof(out));
    CHECK(strcmp(out, "01 02 03 04 05 06 07 08") == 0);
}

static void test_overflow_single_push(void)
{
    uartrx_ring_t r;
    uartrx_ring_reset(&r);
    uint8_t d[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};   // 12 bytes
    uartrx_ring_push(&r, d, 12);
    CHECK_EQ(r.len, 8);       // only the last 8 kept
    CHECK_EQ(r.b[0], 4);
    CHECK_EQ(r.b[7], 11);
}

static void test_slide_after_full(void)
{
    uartrx_ring_t r;
    uartrx_ring_reset(&r);
    uint8_t a[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uartrx_ring_push(&r, a, 8);
    uint8_t b[] = {0xA, 0xB, 0xC};
    uartrx_ring_push(&r, b, 3);
    CHECK_EQ(r.len, 8);       // last 8 = {4,5,6,7,8,A,B,C}
    CHECK_EQ(r.b[0], 4);
    CHECK_EQ(r.b[4], 8);
    CHECK_EQ(r.b[5], 0xA);
    CHECK_EQ(r.b[7], 0xC);
}

static void test_partial_then_small(void)
{
    uartrx_ring_t r;
    uartrx_ring_reset(&r);
    uint8_t a[] = {0x11, 0x22};
    uartrx_ring_push(&r, a, 2);
    uint8_t b[] = {0x33, 0x44, 0x55};
    uartrx_ring_push(&r, b, 3);
    CHECK_EQ(r.len, 5);
    CHECK_EQ(r.b[0], 0x11);
    CHECK_EQ(r.b[4], 0x55);
}

static void test_push_zero_is_noop(void)
{
    uartrx_ring_t r;
    uartrx_ring_reset(&r);
    uint8_t a[] = {0x11};
    uartrx_ring_push(&r, a, 1);
    uartrx_ring_push(&r, a, 0);       // no-op
    uartrx_ring_push(&r, a, -3);      // no-op
    CHECK_EQ(r.len, 1);
    CHECK_EQ(r.b[0], 0x11);
}

static void test_hex_truncates_within_bounds(void)
{
    uartrx_ring_t r;
    uartrx_ring_reset(&r);
    uint8_t d[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uartrx_ring_push(&r, d, 4);
    // Buffer too small for the whole thing: must stay in-bounds + NUL-terminated,
    // truncated to whole bytes ("DE AD" is 5 chars, needs cap >= 6).
    char small[6];
    memset(small, 0x7F, sizeof(small));
    int n = uartrx_ring_hex(&r, small, sizeof(small));
    CHECK(n < (int)sizeof(small));         // fits with room for NUL
    CHECK_EQ(small[n], '\0');              // terminated
    CHECK((int)strlen(small) == n);        // no stray bytes
    CHECK(strcmp(small, "DE AD") == 0);    // whole bytes only
}

static void test_hex_cap_one(void)
{
    uartrx_ring_t r;
    uartrx_ring_reset(&r);
    uint8_t d[] = {0xAB};
    uartrx_ring_push(&r, d, 1);
    char one[1];
    CHECK_EQ(uartrx_ring_hex(&r, one, 1), 0);   // only room for NUL
    CHECK_EQ(one[0], '\0');
}

int main(void)
{
    RUN(test_empty);
    RUN(test_push_small);
    RUN(test_fill_exact);
    RUN(test_overflow_single_push);
    RUN(test_slide_after_full);
    RUN(test_partial_then_small);
    RUN(test_push_zero_is_noop);
    RUN(test_hex_truncates_within_bounds);
    RUN(test_hex_cap_one);
    return REPORT();
}
