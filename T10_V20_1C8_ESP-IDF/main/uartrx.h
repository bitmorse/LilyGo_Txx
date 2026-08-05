// UART RX on GPIO21 (9600 8N1, receive-only) with an explicit two-state machine.
//
//   REST — GPIO21 is a plain GPIO input with NO pull (internal pulls disabled) and
//          the UART peripheral is detached. Nothing reads or drives the line. This
//          is the power-on default and the state we return to when receiving stops.
//   DATA — UART1 is installed and its RX signal is routed onto GPIO21 via the GPIO
//          matrix; a reader task drains incoming bytes. Entered on demand (a UI
//          button), left back to REST on demand.
//
// GPIO21 is free on this board (see docs/HARDWARE.md); it isn't a dedicated UART pin,
// so the RX signal is mapped through the GPIO matrix. UART0 (USB console) is left
// untouched -- this uses UART1.
#pragma once

#include <stdbool.h>

typedef enum { UARTRX_REST, UARTRX_DATA } uartrx_state_t;

// Put GPIO21 into the REST configuration (input, no pull). Call once at boot.
void uartrx_init(void);

void uartrx_start(void);          // REST -> DATA: attach UART1 RX to GPIO21, receive
void uartrx_stop(void);           // DATA -> REST: detach UART, GPIO21 back to input/no-pull

uartrx_state_t uartrx_state(void);
const char *uartrx_state_str(uartrx_state_t s);

// Total bytes received since the last uartrx_start().
unsigned uartrx_bytes(void);

// Hex of the most recently received bytes (up to a few), for the UI. Writes a
// NUL-terminated string into out; returns its length.
int uartrx_last_hex(char *out, int cap);
