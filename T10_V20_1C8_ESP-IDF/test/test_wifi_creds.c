// Unit tests for the BLE WIFI_CREDS payload parser (main/wifi_creds.c).
#include "test_framework.h"
#include "wifi_creds.h"

#include <string.h>

#define S(x) (const unsigned char *)(x)

static void test_basic(void)
{
    char ssid[33], pass[65];
    CHECK(wifi_creds_parse(S("MyWifi\0secret123"), 16, ssid, sizeof ssid, pass, sizeof pass));
    CHECK(strcmp(ssid, "MyWifi") == 0);
    CHECK(strcmp(pass, "secret123") == 0);
}

static void test_open_network_empty_pass(void)
{
    char ssid[33], pass[65];
    // "MyWifi\0" -> ssid, empty pass. len includes the trailing NUL.
    CHECK(wifi_creds_parse(S("MyWifi\0"), 7, ssid, sizeof ssid, pass, sizeof pass));
    CHECK(strcmp(ssid, "MyWifi") == 0);
    CHECK(strcmp(pass, "") == 0);
}

static void test_no_separator_fails(void)
{
    char ssid[33], pass[65];
    CHECK(!wifi_creds_parse(S("MyWifi"), 6, ssid, sizeof ssid, pass, sizeof pass));
}

static void test_empty_ssid_fails(void)
{
    char ssid[33], pass[65];
    CHECK(!wifi_creds_parse(S("\0pass"), 5, ssid, sizeof ssid, pass, sizeof pass));
}

static void test_ssid_max_fits(void)
{
    char ssid[33], pass[65];
    // 32-char SSID + NUL + pass -> fits cap 33.
    unsigned char buf[40];
    memset(buf, 'A', 32); buf[32] = 0; buf[33] = 'p';
    CHECK(wifi_creds_parse(buf, 34, ssid, sizeof ssid, pass, sizeof pass));
    CHECK_EQ((int)strlen(ssid), 32);
    CHECK(strcmp(pass, "p") == 0);
}

static void test_ssid_too_long_fails(void)
{
    char ssid[33], pass[65];
    unsigned char buf[40];
    memset(buf, 'A', 33); buf[33] = 0;      // 33-char SSID doesn't fit cap 33
    CHECK(!wifi_creds_parse(buf, 34, ssid, sizeof ssid, pass, sizeof pass));
}

static void test_pass_too_long_fails(void)
{
    char ssid[33], pass[8];                 // tiny pass cap
    CHECK(!wifi_creds_parse(S("wifi\0longpassword"), 17, ssid, sizeof ssid, pass, sizeof pass));
}

static void test_null_and_zero_len(void)
{
    char ssid[33], pass[65];
    CHECK(!wifi_creds_parse(NULL, 5, ssid, sizeof ssid, pass, sizeof pass));
    CHECK(!wifi_creds_parse(S("x\0y"), 0, ssid, sizeof ssid, pass, sizeof pass));
}

int main(void)
{
    RUN(test_basic);
    RUN(test_open_network_empty_pass);
    RUN(test_no_separator_fails);
    RUN(test_empty_ssid_fails);
    RUN(test_ssid_max_fits);
    RUN(test_ssid_too_long_fails);
    RUN(test_pass_too_long_fails);
    RUN(test_null_and_zero_len);
    return REPORT();
}
