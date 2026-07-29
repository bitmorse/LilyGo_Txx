// Simple square-wave tone / melody player for the onboard speaker (GPIO25),
// using the LEDC (LED PWM) peripheral. Good for chiptune-style boot jingles.
#pragma once

#include <stdint.h>

void sound_init(void);

// Play one tone. freq_hz = 0 means a silent rest. Blocks for duration_ms.
void sound_tone(uint32_t freq_hz, uint32_t duration_ms);

// Play the built-in cute boot melody (blocks ~1.5 s).
void sound_play_boot_melody(void);
