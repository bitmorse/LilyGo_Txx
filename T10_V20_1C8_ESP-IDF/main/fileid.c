#include "fileid.h"
#include <stddef.h>

int fileid_from_uri(const char *uri)
{
    if (uri == NULL) return -1;

    // The last '/'-segment of the PATH -- httpd's req->uri includes the query string,
    // so stop scanning at '?' (query) or '#' (fragment).
    const char *seg = uri;
    for (const char *p = uri; *p && *p != '?' && *p != '#'; p++)
        if (*p == '/') seg = p + 1;

    if (*seg == '\0' || *seg == '?' || *seg == '#') return -1;   // empty segment

    long id = 0;
    for (const char *p = seg; *p && *p != '?' && *p != '#'; p++) {
        if (*p < '0' || *p > '9') return -1;      // any non-digit -> not a file id
        id = id * 10 + (*p - '0');
        if (id > 1000000000L) return -1;          // absurd -> reject (int overflow guard)
    }
    return (int)id;
}
