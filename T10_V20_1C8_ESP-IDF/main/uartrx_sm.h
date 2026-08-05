// Pure state machine for the tool-dock UART RX flow on GPIO21. No ESP-IDF / FreeRTOS
// and no clock calls (time is passed in), so it is fully unit-tested on the host
// (see test/test_uartrx_sm.c). uartrx.c is the hardware glue that feeds it a
// debounced line level + a "real UART byte" flag and performs the returned actions.
//
// Line semantics (docking station + tool PCB over pogo pins):
//   HIGH        = no tool inserted (or UART idle)
//   held LOW    = tool inserted, batteries charging (debounced: LOW >= ~50 ms; a UART
//                 frame never holds LOW that long, so this is unambiguous)
//   UART frames = tool charged and transmitting -> data
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UARTRX_REST,        // idle; GPIO21 input/no-pull, no UART
    UARTRX_WAIT,        // armed; watching the line (HIGH = no tool)
    UARTRX_CHARGING,    // tool inserted (LOW); UART attached, awaiting first data
    UARTRX_DATA,        // real UART data flowing
    UARTRX_FAULT,       // 10 min in CHARGING with no UART -> tool broken
} uartrx_state_t;

// What the hardware glue must do after a transition.
typedef enum {
    UARTRX_ACT_NONE,
    UARTRX_ACT_ATTACH_UART,   // install UART1, route RX <- GPIO21
    UARTRX_ACT_DETACH_UART,   // delete UART, restore GPIO21 to input/no-pull
} uartrx_act_t;

#define UARTRX_CHARGE_TIMEOUT_MS 600000   // 10 min max charge wait before FAULT
#define UARTRX_CHARGE_RELEASE_MS   2000   // line released (HIGH) this long in CHARGING
                                          // with no data -> tool removed -> WAIT (tunable)
#define UARTRX_DATA_IDLE_MS        5000   // no data this long in DATA -> re-arm (tunable)

typedef struct {
    uartrx_state_t state;
    int64_t        since_ms;       // when the current state was entered
    int64_t        last_data_ms;   // last real UART byte (DATA idle re-arm)
    int64_t        low_seen_ms;    // last time the line was LOW in CHARGING (removal grace)
} uartrx_sm_t;

typedef struct {
    bool    line_low;   // GPIO21 held LOW (debounced) -> tool present/charging
    bool    uart_byte;  // a real UART data byte arrived since the last step
    int64_t now_ms;     // monotonic clock in ms
} uartrx_in_t;

void         uartrx_sm_init(uartrx_sm_t *sm, int64_t now_ms);   // -> REST
uartrx_act_t uartrx_sm_start(uartrx_sm_t *sm, int64_t now_ms);  // REST -> WAIT
uartrx_act_t uartrx_sm_stop(uartrx_sm_t *sm, int64_t now_ms);   // any -> REST
uartrx_act_t uartrx_sm_step(uartrx_sm_t *sm, uartrx_in_t in);

const char  *uartrx_state_str(uartrx_state_t s);
