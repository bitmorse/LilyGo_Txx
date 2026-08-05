// Unit tests for the tool-dock UART RX state machine (main/uartrx_sm.c). Pure host
// tests -- no ESP-IDF; time is passed in, so timeouts are exercised deterministically.
#include "test_framework.h"
#include "uartrx_sm.h"

#include <string.h>

static uartrx_in_t IN(bool low, bool byte, int64_t now)
{
    uartrx_in_t i = { low, byte, now };
    return i;
}

// Drive REST -> WAIT -> CHARGING (line low at t). Returns with sm in CHARGING.
static void to_charging(uartrx_sm_t *sm, int64_t t)
{
    uartrx_sm_init(sm, 0);
    uartrx_sm_start(sm, 0);
    uartrx_act_t a = uartrx_sm_step(sm, (uartrx_in_t){ .line_low = true, .now_ms = t });
    CHECK_EQ(sm->state, UARTRX_CHARGING);
    CHECK_EQ(a, UARTRX_ACT_ATTACH_UART);
}

static void test_init_is_rest(void)
{
    uartrx_sm_t sm;
    uartrx_sm_init(&sm, 100);
    CHECK_EQ(sm.state, UARTRX_REST);
}

static void test_start_rest_to_wait(void)
{
    uartrx_sm_t sm;
    uartrx_sm_init(&sm, 0);
    CHECK_EQ(uartrx_sm_start(&sm, 0), UARTRX_ACT_NONE);
    CHECK_EQ(sm.state, UARTRX_WAIT);
}

static void test_start_ignored_when_armed(void)
{
    uartrx_sm_t sm;
    uartrx_sm_init(&sm, 0);
    uartrx_sm_start(&sm, 0);
    uartrx_sm_start(&sm, 5);          // no-op
    CHECK_EQ(sm.state, UARTRX_WAIT);
}

static void test_wait_high_stays(void)
{
    uartrx_sm_t sm;
    uartrx_sm_init(&sm, 0);
    uartrx_sm_start(&sm, 0);
    uartrx_act_t a = uartrx_sm_step(&sm, IN(false, false, 10));
    CHECK_EQ(sm.state, UARTRX_WAIT);
    CHECK_EQ(a, UARTRX_ACT_NONE);
}

static void test_wait_low_to_charging_attaches(void)
{
    uartrx_sm_t sm;
    to_charging(&sm, 10);             // asserts CHARGING + ATTACH inside
}

static void test_charging_byte_to_data(void)
{
    uartrx_sm_t sm;
    to_charging(&sm, 10);
    uartrx_act_t a = uartrx_sm_step(&sm, IN(false, true, 20));
    CHECK_EQ(sm.state, UARTRX_DATA);
    CHECK_EQ(a, UARTRX_ACT_NONE);
}

static void test_charging_no_byte_stays(void)
{
    uartrx_sm_t sm;
    to_charging(&sm, 10);
    uartrx_act_t a = uartrx_sm_step(&sm, IN(true, false, 20));
    CHECK_EQ(sm.state, UARTRX_CHARGING);
    CHECK_EQ(a, UARTRX_ACT_NONE);
}

static void test_charging_timeout_boundary(void)
{
    uartrx_sm_t sm;
    to_charging(&sm, 10);
    // Just under 10 min from entry (entered at t=10): still CHARGING.
    uartrx_act_t a = uartrx_sm_step(&sm, IN(true, false, 10 + UARTRX_CHARGE_TIMEOUT_MS - 1));
    CHECK_EQ(sm.state, UARTRX_CHARGING);
    CHECK_EQ(a, UARTRX_ACT_NONE);
    // At exactly 10 min: FAULT + detach.
    a = uartrx_sm_step(&sm, IN(true, false, 10 + UARTRX_CHARGE_TIMEOUT_MS));
    CHECK_EQ(sm.state, UARTRX_FAULT);
    CHECK_EQ(a, UARTRX_ACT_DETACH_UART);
}

static void test_data_byte_keeps_alive(void)
{
    uartrx_sm_t sm;
    to_charging(&sm, 10);
    uartrx_sm_step(&sm, IN(false, true, 20));           // -> DATA
    // A byte well past the idle window still keeps it in DATA (refreshes activity).
    uartrx_act_t a = uartrx_sm_step(&sm, IN(false, true, 20 + 10 * UARTRX_DATA_IDLE_MS));
    CHECK_EQ(sm.state, UARTRX_DATA);
    CHECK_EQ(a, UARTRX_ACT_NONE);
}

static void test_data_idle_rearms(void)
{
    uartrx_sm_t sm;
    to_charging(&sm, 10);
    uartrx_sm_step(&sm, IN(false, true, 20));           // -> DATA, last_data=20
    // Just under idle: stays DATA.
    uartrx_act_t a = uartrx_sm_step(&sm, IN(false, false, 20 + UARTRX_DATA_IDLE_MS - 1));
    CHECK_EQ(sm.state, UARTRX_DATA);
    // At idle threshold: re-arm to WAIT + detach.
    a = uartrx_sm_step(&sm, IN(false, false, 20 + UARTRX_DATA_IDLE_MS));
    CHECK_EQ(sm.state, UARTRX_WAIT);
    CHECK_EQ(a, UARTRX_ACT_DETACH_UART);
}

static void test_fault_rearms_on_removal(void)
{
    uartrx_sm_t sm;
    to_charging(&sm, 10);
    uartrx_sm_step(&sm, IN(true, false, 10 + UARTRX_CHARGE_TIMEOUT_MS));  // -> FAULT
    CHECK_EQ(sm.state, UARTRX_FAULT);
    // Still low (tool stuck): stays FAULT.
    uartrx_sm_step(&sm, IN(true, false, 10 + UARTRX_CHARGE_TIMEOUT_MS + 100));
    CHECK_EQ(sm.state, UARTRX_FAULT);
    // Tool removed (line high): re-arm to WAIT.
    uartrx_act_t a = uartrx_sm_step(&sm, IN(false, false, 10 + UARTRX_CHARGE_TIMEOUT_MS + 200));
    CHECK_EQ(sm.state, UARTRX_WAIT);
    CHECK_EQ(a, UARTRX_ACT_NONE);
}

static void test_stop_detaches_when_attached(void)
{
    uartrx_sm_t sm;
    to_charging(&sm, 10);
    CHECK_EQ(uartrx_sm_stop(&sm, 30), UARTRX_ACT_DETACH_UART);
    CHECK_EQ(sm.state, UARTRX_REST);

    to_charging(&sm, 10);
    uartrx_sm_step(&sm, IN(false, true, 20));           // -> DATA
    CHECK_EQ(uartrx_sm_stop(&sm, 40), UARTRX_ACT_DETACH_UART);
    CHECK_EQ(sm.state, UARTRX_REST);
}

static void test_stop_from_wait_no_detach(void)
{
    uartrx_sm_t sm;
    uartrx_sm_init(&sm, 0);
    uartrx_sm_start(&sm, 0);
    CHECK_EQ(uartrx_sm_stop(&sm, 5), UARTRX_ACT_NONE);   // nothing attached in WAIT
    CHECK_EQ(sm.state, UARTRX_REST);
}

static void test_full_cycle(void)
{
    uartrx_sm_t sm;
    uartrx_sm_init(&sm, 0);
    CHECK_EQ(uartrx_sm_start(&sm, 0), UARTRX_ACT_NONE);
    CHECK_EQ(uartrx_sm_step(&sm, IN(true, false, 100)), UARTRX_ACT_ATTACH_UART);   // WAIT->CHARGING
    CHECK_EQ(uartrx_sm_step(&sm, IN(false, true, 200)), UARTRX_ACT_NONE);          // CHARGING->DATA
    CHECK_EQ(uartrx_sm_step(&sm, IN(false, false, 200 + UARTRX_DATA_IDLE_MS)),
             UARTRX_ACT_DETACH_UART);                                               // DATA->WAIT
    CHECK_EQ(sm.state, UARTRX_WAIT);
    // Next tool docks -> attach again.
    CHECK_EQ(uartrx_sm_step(&sm, IN(true, false, 999999)), UARTRX_ACT_ATTACH_UART);
    CHECK_EQ(sm.state, UARTRX_CHARGING);
}

static void test_state_str(void)
{
    CHECK(strcmp(uartrx_state_str(UARTRX_REST), "REST") == 0);
    CHECK(strcmp(uartrx_state_str(UARTRX_WAIT), "WAIT") == 0);
    CHECK(strcmp(uartrx_state_str(UARTRX_CHARGING), "CHARGING") == 0);
    CHECK(strcmp(uartrx_state_str(UARTRX_DATA), "DATA") == 0);
    CHECK(strcmp(uartrx_state_str(UARTRX_FAULT), "FAULT") == 0);
}

int main(void)
{
    RUN(test_init_is_rest);
    RUN(test_start_rest_to_wait);
    RUN(test_start_ignored_when_armed);
    RUN(test_wait_high_stays);
    RUN(test_wait_low_to_charging_attaches);
    RUN(test_charging_byte_to_data);
    RUN(test_charging_no_byte_stays);
    RUN(test_charging_timeout_boundary);
    RUN(test_data_byte_keeps_alive);
    RUN(test_data_idle_rearms);
    RUN(test_fault_rearms_on_removal);
    RUN(test_stop_detaches_when_attached);
    RUN(test_stop_from_wait_no_detach);
    RUN(test_full_cycle);
    RUN(test_state_str);
    return REPORT();
}
