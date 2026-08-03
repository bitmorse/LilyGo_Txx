// Persistent user settings (stored in NVS). Load once at boot with
// settings_init() (after NVS is initialised by provisioning_hw_init()).
#pragma once

#include <stdbool.h>

void settings_init(void);                  // load cached values from NVS

bool settings_boot_sound(void);            // play the chiptune on boot?
void settings_set_boot_sound(bool on);     // persist + update the cache
