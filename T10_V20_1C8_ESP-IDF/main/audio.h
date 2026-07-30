// WAV clip playback through the onboard speaker (GPIO25 = DAC1).
// Clips are embedded from audio_clips/*.wav via tools/wav2c.py (make audio).
#pragma once

int         audio_clip_count(void);
const char *audio_clip_name(int index);

// Play the clip once (non-blocking; runs in a background task). Ignored if a
// clip is already playing.
void audio_play(int index);
