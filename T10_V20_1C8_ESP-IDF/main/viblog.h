// Industrial vibration logger: capture the MPU9250 accelerometer at its 4 kHz /
// +/-16 g maximum and stream it to the microSD card as an MCAP file (protobuf,
// batched) for offline FFT / vibration analysis in Foxglove Studio or numpy.
//
// Pipeline: [imu FIFO @4kHz] -> sampler task (core1) -> StreamBuffer ->
//           writer task (core0) -> MCAP file on /sdcard.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     running;
    char     path[40];      // /sdcard/vibNNNN.mcap
    uint32_t rate_hz;       // nominal capture rate (4000)
    uint64_t samples;       // accel samples written to the file
    uint32_t drops;         // samples lost (FIFO overflow / buffer full)
    uint32_t elapsed_s;     // seconds since start
    uint32_t buf_pct;       // StreamBuffer fill level, 0..100
    uint64_t bytes;         // bytes written to the file
    uint64_t sd_free_mb;    // free space on the card, MB
    bool     time_synced;   // NTP wall-clock available (else monotonic stamps)
    char     err[48];       // last error, empty if none
} viblog_status_t;

// Mount SD, open the next vibNNNN.mcap, start capture. Returns false on error
// (err in the status has the reason). No-op / true if already running.
bool viblog_start(void);

// Stop capture, flush + close the file, unmount the card. Blocks until done.
void viblog_stop(void);

bool viblog_is_running(void);

// Snapshot the live status (safe to call from the UI timer).
void viblog_get_status(viblog_status_t *out);
