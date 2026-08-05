#include "wifi_creds.h"

#include <string.h>

bool wifi_creds_parse(const unsigned char *buf, int len,
                      char *ssid, int ssid_cap, char *pass, int pass_cap)
{
    if (!buf || len <= 0 || ssid_cap < 1 || pass_cap < 1) return false;

    int sep = -1;                          // index of the NUL separator
    for (int i = 0; i < len; i++)
        if (buf[i] == 0) { sep = i; break; }
    if (sep <= 0) return false;            // no separator, or empty SSID
    if (sep >= ssid_cap) return false;     // SSID doesn't fit (with its NUL)

    int plen = len - sep - 1;              // bytes after the separator
    if (plen < 0) plen = 0;
    if (plen >= pass_cap) return false;    // passphrase doesn't fit

    memcpy(ssid, buf, (size_t)sep);        ssid[sep]  = '\0';
    memcpy(pass, buf + sep + 1, (size_t)plen); pass[plen] = '\0';
    return true;
}
