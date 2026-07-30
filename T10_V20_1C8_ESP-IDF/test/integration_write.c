// Integration harness: write a real MCAP file using the *production* units
// (main/mcap.c + main/accel_encode.c + the committed schema descriptor), so the
// output can be validated against the official `mcap` + protobuf libraries.
// Writes both channels: /accel (AccelBatch) and /imu (ImuAux).
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
    if (!mcap_begin(&w, f)) return 3;
    // Both schemas embed the same FileDescriptorSet, referenced by message name.
    mcap_add_schema(&w, 1, ACCEL_SCHEMA_NAME, "protobuf",
                    accel_schema_fds, accel_schema_fds_len);
    mcap_add_schema(&w, 2, IMUAUX_SCHEMA_NAME, "protobuf",
                    accel_schema_fds, accel_schema_fds_len);
    mcap_add_channel(&w, 1, 1, "/accel", "protobuf");
    mcap_add_channel(&w, 2, 2, "/imu", "protobuf");

    enum { AN = 500, IN = 50, BATCHES = 4, ARATE = 4000, IRATE = 100 };
    float x[AN], y[AN], z[AN];
    float g[IN], m[IN], tp[IN];
    uint8_t amsg[ACCEL_MSG_MAXLEN(AN)];
    uint8_t imsg[IMUAUX_MSG_MAXLEN(IN)];

    for (int b = 0; b < BATCHES; b++) {
        for (int i = 0; i < AN; i++) {
            float t = (float)(b * AN + i) / ARATE;
            x[i] = sinf(t * 2.0f * (float)M_PI * 50.0f);
            y[i] = 0.5f * cosf(t * 2.0f * (float)M_PI * 120.0f);
            z[i] = 1.0f;
        }
        uint64_t at0 = 1700000000000000000ULL + (uint64_t)(b * AN) * 250000ULL;
        uint32_t al = accel_encode_batch(amsg, sizeof(amsg), at0, ARATE, x, y, z, AN);
        if (al == 0 || !mcap_write_message(&w, 1, at0, at0, amsg, al)) return 4;

        for (int i = 0; i < IN; i++) { g[i] = (float)i; m[i] = 20.0f; tp[i] = 25.0f; }
        uint64_t it0 = 1700000000000000000ULL + (uint64_t)(b * IN) * 10000000ULL;
        uint32_t il = imuaux_encode_batch(imsg, sizeof(imsg), it0, IRATE,
                                          g, g, g, m, m, m, tp, IN);
        if (il == 0 || !mcap_write_message(&w, 2, it0, it0, imsg, il)) return 5;
    }
    if (!mcap_close(&w)) return 6;
    fclose(f);
    printf("wrote %s (%d accel + %d imu batches)\n", path, BATCHES, BATCHES);
    return 0;
}
