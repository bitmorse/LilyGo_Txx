// Tool-dock UART RX on GPIO21 (9600 8N1, receive-only). Hardware glue around the
// pure state machine in uartrx_sm.h -- a monitor task polls the debounced line level
// and (once a tool is detected) the UART, driving the machine and attaching/detaching
// UART1 as it directs.
//
//   REST     — GPIO21 input, no pull, no UART. Power-on default.
//   WAIT     — armed; line HIGH = no tool inserted.
//   CHARGING — line held LOW = tool inserted, batteries charging; awaiting UART.
//   DATA     — UART data flowing; bytes counted + last bytes shown.
//   FAULT    — 10 min charging with no UART = tool broken.
//
// GPIO21 is free on this board (docs/HARDWARE.md); not a dedicated UART pin, so RX is
// mapped through the GPIO matrix onto UART1 (UART0 is the USB console, left untouched).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "uartrx_sm.h"          // uartrx_state_t, uartrx_state_str()

// Put GPIO21 into REST (input, no pull). Call once at boot.
void uartrx_init(void);

void uartrx_start(void);        // REST -> WAIT: arm; begin watching for a docked tool
void uartrx_stop(void);         // any -> REST: disarm, detach UART, GPIO21 input/no-pull

uartrx_state_t uartrx_state(void);

// Milliseconds spent in the current state (for the CHARGING countdown display).
int64_t uartrx_state_elapsed_ms(void);

// Total bytes received since entering DATA.
unsigned uartrx_bytes(void);

// Hex of the most recently received bytes (up to a few), for the UI. Writes a
// NUL-terminated string into out; returns its length.
int uartrx_last_hex(char *out, int cap);

// The session is recorded to /sdcard/uartNNNN.mcap (channels /uart_rx raw bytes,
// /state, /meta). "" if no SD card / not recording. Bytes written so far.
const char *uartrx_rec_path(void);
uint64_t    uartrx_rec_bytes(void);

// True while the MCAP recording file is open (actively writing the SD card). The
// connectivity manager refuses a file-sync session while this is true -- one SD
// writer at a time (same rule as viblog_is_running()).
bool uartrx_is_recording(void);
