// Unit tests for the UART RX MCAP /state and /meta JSON line builders
// (main/uartrx_rec.c). Pure host tests -- no ESP-IDF.
#include "test_framework.h"
#include "uartrx_rec.h"

#include <string.h>

static void test_state_json_basic(void)
{
    char out[96];
    int n = uartrx_rec_state_json(out, sizeof(out), "CHARGING", 123, 4567);
    CHECK(strcmp(out, "{\"state\":\"CHARGING\",\"bytes\":123,\"elapsed_ms\":4567}") == 0);
    CHECK_EQ(n, (int)strlen(out));
}

static void test_state_json_zeros(void)
{
    char out[96];
    uartrx_rec_state_json(out, sizeof(out), "REST", 0, 0);
    CHECK(strcmp(out, "{\"state\":\"REST\",\"bytes\":0,\"elapsed_ms\":0}") == 0);
}

static void test_meta_json_unsynced(void)
{
    char out[128];
    int n = uartrx_rec_meta_json(out, sizeof(out), "v1.2", "T10_AABBCC", 9600, 21, false);
    CHECK(strcmp(out,
        "{\"fw\":\"v1.2\",\"dev\":\"T10_AABBCC\",\"baud\":9600,\"gpio\":21,\"time_synced\":false}") == 0);
    CHECK_EQ(n, (int)strlen(out));
}

static void test_meta_json_synced(void)
{
    char out[128];
    uartrx_rec_meta_json(out, sizeof(out), "v1", "T10_X", 115200, 21, true);
    CHECK(strcmp(out,
        "{\"fw\":\"v1\",\"dev\":\"T10_X\",\"baud\":115200,\"gpio\":21,\"time_synced\":true}") == 0);
}

static void test_truncation_is_safe(void)
{
    char small[10];
    memset(small, 0x7F, sizeof(small));
    int n = uartrx_rec_state_json(small, sizeof(small), "CHARGING", 123, 4567);
    CHECK(n < (int)sizeof(small));          // fits with room for NUL
    CHECK_EQ(small[n], '\0');               // NUL-terminated
    CHECK_EQ((int)strlen(small), n);        // no stray bytes past the NUL
}

int main(void)
{
    RUN(test_state_json_basic);
    RUN(test_state_json_zeros);
    RUN(test_meta_json_unsynced);
    RUN(test_meta_json_synced);
    RUN(test_truncation_is_safe);
    return REPORT();
}
