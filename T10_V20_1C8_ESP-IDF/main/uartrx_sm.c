#include "uartrx_sm.h"

static void enter(uartrx_sm_t *sm, uartrx_state_t st, int64_t now)
{
    sm->state = st;
    sm->since_ms = now;
}

void uartrx_sm_init(uartrx_sm_t *sm, int64_t now_ms)
{
    sm->last_data_ms = now_ms;
    sm->low_seen_ms  = now_ms;
    enter(sm, UARTRX_REST, now_ms);
}

uartrx_act_t uartrx_sm_start(uartrx_sm_t *sm, int64_t now_ms)
{
    if (sm->state == UARTRX_REST) enter(sm, UARTRX_WAIT, now_ms);
    return UARTRX_ACT_NONE;
}

uartrx_act_t uartrx_sm_stop(uartrx_sm_t *sm, int64_t now_ms)
{
    if (sm->state == UARTRX_REST) return UARTRX_ACT_NONE;
    bool attached = (sm->state == UARTRX_CHARGING || sm->state == UARTRX_DATA);
    enter(sm, UARTRX_REST, now_ms);
    return attached ? UARTRX_ACT_DETACH_UART : UARTRX_ACT_NONE;
}

uartrx_act_t uartrx_sm_step(uartrx_sm_t *sm, uartrx_in_t in)
{
    switch (sm->state) {
    case UARTRX_WAIT:
        if (in.line_low) {                       // tool inserted, holding the line low
            sm->low_seen_ms = in.now_ms;
            enter(sm, UARTRX_CHARGING, in.now_ms);
            return UARTRX_ACT_ATTACH_UART;
        }
        return UARTRX_ACT_NONE;

    case UARTRX_CHARGING:
        if (in.uart_byte) {                      // first real data -> record from here
            sm->last_data_ms = in.now_ms;
            enter(sm, UARTRX_DATA, in.now_ms);
            return UARTRX_ACT_NONE;
        }
        if (in.line_low) {
            sm->low_seen_ms = in.now_ms;         // still holding low -> keep charging
        } else if (in.now_ms - sm->low_seen_ms >= UARTRX_CHARGE_RELEASE_MS) {
            enter(sm, UARTRX_WAIT, in.now_ms);   // released with no data -> tool removed
            return UARTRX_ACT_DETACH_UART;
        }
        if (in.now_ms - sm->since_ms >= UARTRX_CHARGE_TIMEOUT_MS) {
            enter(sm, UARTRX_FAULT, in.now_ms);  // battery never charged / tool broken
            return UARTRX_ACT_DETACH_UART;
        }
        return UARTRX_ACT_NONE;

    case UARTRX_DATA:
        if (in.uart_byte) {                      // keep alive
            sm->last_data_ms = in.now_ms;
            return UARTRX_ACT_NONE;
        }
        if (in.now_ms - sm->last_data_ms >= UARTRX_DATA_IDLE_MS) {
            enter(sm, UARTRX_WAIT, in.now_ms);   // data stopped / tool removed -> re-arm
            return UARTRX_ACT_DETACH_UART;
        }
        return UARTRX_ACT_NONE;

    case UARTRX_FAULT:
        if (!in.line_low) {                      // tool removed -> ready for the next one
            enter(sm, UARTRX_WAIT, in.now_ms);
        }
        return UARTRX_ACT_NONE;

    case UARTRX_REST:
    default:
        return UARTRX_ACT_NONE;
    }
}

const char *uartrx_state_str(uartrx_state_t s)
{
    switch (s) {
    case UARTRX_REST:     return "REST";
    case UARTRX_WAIT:     return "WAIT";
    case UARTRX_CHARGING: return "CHARGING";
    case UARTRX_DATA:     return "DATA";
    case UARTRX_FAULT:    return "FAULT";
    default:              return "?";
    }
}
