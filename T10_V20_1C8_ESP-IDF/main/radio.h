// Internet-radio prototype: stream + decode MP3 to the speaker (GPIO25 DAC).
#pragma once

#include <stdbool.h>

void        radio_toggle(void);      // start if stopped, stop if playing
bool        radio_is_playing(void);
const char *radio_status(void);      // "Off" / "Buffering" / "Playing" / "Error"
