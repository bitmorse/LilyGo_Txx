#pragma once

// Best-effort: tell the onboard IP5306 power chip to keep its boost converter on
// so the board doesn't auto power-off when running from a LiPo battery. Harmless
// (and unnecessary) when powered over USB. Failures are ignored.
void power_init(void);
