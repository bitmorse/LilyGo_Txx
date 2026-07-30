// Three onboard buttons on this T10 V2.0: GPIO 35 / 34 / 39 (found empirically;
// the repo's Arduino header lists 36/37/39, wrong for this revision).
// These are input-only pins with NO internal pulls; the board provides external
// pull-ups, so a press reads LOW (active-low).
#pragma once

#define BTN_1 0   // GPIO35
#define BTN_2 1   // GPIO34
#define BTN_3 2   // GPIO39

void buttons_init(void);

// Poll once. Returns a bitmask of buttons that transitioned to pressed since the
// last call: bit (1<<BTN_x). Call this periodically (e.g. every 20-30 ms).
int buttons_poll(void);

// Current state of one button: 1 = pressed (active-low), 0 = released.
int buttons_level(int idx);
