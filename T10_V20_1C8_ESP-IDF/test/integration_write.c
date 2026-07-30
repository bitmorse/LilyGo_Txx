// Integration harness: write a real MCAP file using the *production* units
// (main/mcap.c + main/accel_encode.c + the committed schema descriptor), so the
// output can be validated against the official `mcap` + protobuf libraries.
// Usage: integration_write <out.mcap>
#include "mcap.h"
#include "accel_encode.h"
#include "accel_schema.h"

#include <math.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/tmp/viblog_test.mcap";
    FILE *f = fopen(path, "wb");
    if (!f) return 2;

    mcap_writer_t w;
    if (!mcap_open(&w, f, "/accel", "protobuf", ACCEL_SCHEMA_NAME, "protobuf",
                   accel_schema_fds, accel_schema_fds_len))
        return 3;

    enum { N = 500, BATCHES = 4, RATE = 4000 };
    float x[N], y[N], z[N];
    uint8_t msg[ACCEL_MSG_MAXLEN(N)];

    for (int b = 0; b < BATCHES; b++) {
        for (int i = 0; i < N; i++) {
            float t = (float)(b * N + i) / RATE;
            x[i] = sinf(t * 2.0f * (float)M_PI * 50.0f);   // 50 Hz tone
            y[i] = 0.5f * cosf(t * 2.0f * (float)M_PI * 120.0f);
            z[i] = 1.0f;                                    // ~1 g gravity
        }
        uint64_t t0 = 1700000000000000000ULL + (uint64_t)(b * N) * 250000ULL;
        uint32_t len = accel_encode_batch(msg, sizeof(msg), t0, RATE, x, y, z, N);
        if (len == 0) return 4;
        if (!mcap_write_message(&w, t0, t0, msg, len)) return 5;
    }
    if (!mcap_close(&w)) return 6;
    fclose(f);
    printf("wrote %s (%d batches, %d samples)\n", path, BATCHES, BATCHES * N);
    return 0;
}
