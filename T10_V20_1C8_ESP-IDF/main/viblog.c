#include "viblog.h"
#include "imu.h"
#include "sdcard.h"
#include "mcap.h"
#include "accel_encode.h"        // accel_encode_batch(), ACCEL_MSG_MAXLEN()
#include "provisioning.h"        // time_now_ns(), time_is_synced()
#include "accel_schema.h"        // accel_schema_fds[], ACCEL_SCHEMA_NAME

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"

static const char *TAG = "viblog";

// --- capture parameters -----------------------------------------------------
#define RATE_HZ      4000
#define DT_NS        (1000000000ULL / RATE_HZ)   // 250000 ns / sample
#define SAMPLE_BYTES 6                            // int16 x,y,z
#define BATCH        500                          // samples per MCAP message
#define SB_BYTES     (32 * 1024)                  // StreamBuffer (~1.3 s @4kHz)
#define FIFO_MAX     85                           // MPU9250 FIFO / 6 bytes
#define MSG_CAP      ACCEL_MSG_MAXLEN(BATCH)      // encode buffer capacity

// --- state ------------------------------------------------------------------
static StreamBufferHandle_t s_sb;
static TaskHandle_t s_sampler, s_writer;
static volatile bool s_run;
static FILE        *s_fp;
static mcap_writer_t s_mcap;
static uint64_t s_t0_ns;             // capture epoch (sample 0)
static float    s_lsb_per_g = 2048.0f;
static uint64_t s_sd_free0;          // SD free bytes at start (cached, not polled)

// Writer-owned scratch (allocated in viblog_start so OOM is handled there, freed
// by the writer task on exit). Kept off the hot path's stack.
static float   *s_x, *s_y, *s_z;
static uint8_t *s_msg;

// counters: each written by exactly one task (sampler or writer) -> no locks
static volatile uint64_t s_captured;  // sampler: samples pushed to buffer
static volatile uint32_t s_drops;     // sampler: samples lost
static volatile uint64_t s_written;   // writer: samples written to file
static volatile uint64_t s_bytes;     // writer: bytes written

static char s_path[40];
static char s_err[48];

bool viblog_is_running(void) { return s_run; }

// --- sampler task (core 1): drain the IMU FIFO into the StreamBuffer ---------

static void sampler_task(void *arg)
{
    (void)arg;
    int16_t buf[FIFO_MAX * 3];
    while (s_run) {
        bool ovf = false;
        int n = imu_hires_read(buf, FIFO_MAX, &ovf);
        if (ovf) s_drops += FIFO_MAX;               // nominal (true count unknown)
        if (n > 0) {
            size_t bytes = (size_t)n * SAMPLE_BYTES;
            // Only push whole reads so the 6-byte framing in the buffer is exact.
            if (xStreamBufferSpacesAvailable(s_sb) >= bytes) {
                xStreamBufferSend(s_sb, buf, bytes, 0);
                s_captured += n;
            } else {
                s_drops += n;                       // writer/SD fell behind
            }
            if (n >= FIFO_MAX - 4) continue;        // FIFO was full: drain again
        }
        vTaskDelay(pdMS_TO_TICKS(2));               // ~8 samples accumulate
    }
    s_sampler = NULL;
    vTaskDelete(NULL);
}

// --- writer task (core 0): batch, protobuf-encode, write MCAP messages -------

static void write_batch(float *x, float *y, float *z, uint8_t *msg,
                        uint64_t base_index, int n)
{
    assert(x && y && z && msg && n > 0 && n <= BATCH);
    uint64_t t0 = s_t0_ns + base_index * DT_NS;
    uint32_t len = accel_encode_batch(msg, MSG_CAP, t0, RATE_HZ, x, y, z, n);
    if (len == 0) {                                 // encode/bounds error
        if (s_err[0] == '\0') snprintf(s_err, sizeof(s_err), "encode error");
        return;
    }
    if (mcap_write_message(&s_mcap, t0, t0, msg, len)) {
        s_written += n;
        s_bytes   += len + 22;                      // + MCAP message record header
    } else if (s_err[0] == '\0') {
        snprintf(s_err, sizeof(s_err), "SD write failed");
    }
}

static void writer_task(void *arg)
{
    (void)arg;
    uint8_t  rbuf[256 * SAMPLE_BYTES];
    int      filled = 0;
    uint64_t base_index = 0;                        // sample index of batch start

    while (s_run || xStreamBufferBytesAvailable(s_sb) >= SAMPLE_BYTES) {
        size_t got = xStreamBufferReceive(s_sb, rbuf, sizeof(rbuf),
                                          pdMS_TO_TICKS(100));
        int ns = (int)(got / SAMPLE_BYTES);
        for (int i = 0; i < ns; i++) {
            int16_t t[3];                               // memcpy: no aliasing/align UB
            memcpy(t, &rbuf[i * SAMPLE_BYTES], SAMPLE_BYTES);
            s_x[filled] = t[0] / s_lsb_per_g;
            s_y[filled] = t[1] / s_lsb_per_g;
            s_z[filled] = t[2] / s_lsb_per_g;
            if (++filled == BATCH) {
                write_batch(s_x, s_y, s_z, s_msg, base_index, filled);
                base_index += filled;
                filled = 0;
            }
        }
    }
    if (filled > 0)                                 // final partial batch
        write_batch(s_x, s_y, s_z, s_msg, base_index, filled);

    mcap_close(&s_mcap);
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
    free(s_x); free(s_y); free(s_z); free(s_msg);
    s_x = s_y = s_z = NULL; s_msg = NULL;
    ESP_LOGI(TAG, "writer done: %llu samples, %llu bytes",
             (unsigned long long)s_written, (unsigned long long)s_bytes);
    s_writer = NULL;
    vTaskDelete(NULL);
}

// --- start / stop -----------------------------------------------------------

static bool next_path(char *out, int n)
{
    for (int i = 0; i < 10000; i++) {
        snprintf(out, n, "/sdcard/vib%04d.mcap", i);
        struct stat st;
        if (stat(out, &st) != 0) return true;       // doesn't exist -> use it
    }
    return false;
}

bool viblog_start(void)
{
    if (s_run) return true;
    s_err[0] = '\0';

    if (!imu_hires_start()) {                        // needs the MPU9250
        snprintf(s_err, sizeof(s_err), "no accelerometer");
        return false;
    }
    if (!sd_mount()) {
        snprintf(s_err, sizeof(s_err), "no SD card");
        imu_hires_stop();
        return false;
    }
    if (!next_path(s_path, sizeof(s_path))) {
        snprintf(s_err, sizeof(s_err), "card full of logs");
        goto fail;
    }
    s_fp = fopen(s_path, "wb");
    if (!s_fp) {
        snprintf(s_err, sizeof(s_err), "open failed");
        goto fail;
    }
    setvbuf(s_fp, NULL, _IOFBF, 16 * 1024);          // fewer, larger SD writes

    s_sb = xStreamBufferCreate(SB_BYTES, SAMPLE_BYTES);
    s_x = malloc(BATCH * sizeof(float));
    s_y = malloc(BATCH * sizeof(float));
    s_z = malloc(BATCH * sizeof(float));
    s_msg = malloc(MSG_CAP);                         // worst-case encoded batch
    if (!s_sb || !s_x || !s_y || !s_z || !s_msg) {
        snprintf(s_err, sizeof(s_err), "no memory");
        goto fail;
    }

    if (!mcap_open(&s_mcap, s_fp, "/accel", "protobuf",
                   ACCEL_SCHEMA_NAME, "protobuf",
                   accel_schema_fds, accel_schema_fds_len)) {
        snprintf(s_err, sizeof(s_err), "mcap header failed");
        goto fail;
    }

    s_lsb_per_g = imu_hires_lsb_per_g();
    s_captured = s_drops = s_written = s_bytes = 0;
    s_sd_free0 = sd_free_bytes();                    // cache once; don't poll live
    s_t0_ns = time_now_ns();
    s_run = true;

    // Writer on core 0 (with WiFi/LVGL); sampler on core 1 at higher priority so
    // FIFO draining is never starved by the SD/write path.
    xTaskCreatePinnedToCore(writer_task,  "vibwr", 6144, NULL, 5, &s_writer,  0);
    xTaskCreatePinnedToCore(sampler_task, "vibsm", 4096, NULL, 6, &s_sampler, 1);

    ESP_LOGI(TAG, "logging to %s", s_path);
    return true;

fail:
    if (s_sb) { vStreamBufferDelete(s_sb); s_sb = NULL; }
    free(s_x); free(s_y); free(s_z); free(s_msg);
    s_x = s_y = s_z = NULL; s_msg = NULL;
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
    imu_hires_stop();
    sd_unmount();
    return false;
}

void viblog_stop(void)
{
    if (!s_run) return;
    s_run = false;                                   // both tasks observe this

    // Wait for the tasks to finish (writer flushes + closes the file).
    for (int i = 0; i < 200 && (s_writer || s_sampler); i++)
        vTaskDelay(pdMS_TO_TICKS(20));               // up to ~4 s

    imu_hires_stop();
    if (s_sb) { vStreamBufferDelete(s_sb); s_sb = NULL; }
    sd_unmount();
    ESP_LOGI(TAG, "stopped");
}

void viblog_get_status(viblog_status_t *o)
{
    if (!o) return;
    memset(o, 0, sizeof(*o));
    o->running     = s_run;
    o->rate_hz     = RATE_HZ;
    o->samples     = s_written;
    o->drops       = s_drops;
    o->bytes       = s_bytes;
    o->time_synced = time_is_synced();
    snprintf(o->path, sizeof(o->path), "%s", s_path);
    snprintf(o->err,  sizeof(o->err),  "%s", s_err);
    if (s_run && s_t0_ns)
        o->elapsed_s = (uint32_t)((time_now_ns() - s_t0_ns) / 1000000000ULL);
    if (s_sb)
        o->buf_pct = (uint32_t)(xStreamBufferBytesAvailable(s_sb) * 100 / SB_BYTES);
    // Estimate free space from the start snapshot minus bytes written, rather than
    // running f_getfree live (it would contend with the writer on the SD bus).
    uint64_t used = s_bytes;
    o->sd_free_mb = (s_sd_free0 > used ? s_sd_free0 - used : 0) >> 20;
}
