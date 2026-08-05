// Pure JSON line builders for the UART RX MCAP recording (the /state and /meta
// channels). No ESP-IDF deps, so they're unit-tested on the host. The raw /uart_rx
// bytes need no formatting -- they're written verbatim -- so only these two small
// formatters are non-trivial logic. Inputs are trusted, simple values (fixed state
// names, hex device id, numeric fields), so no JSON string-escaping is done.
#pragma once

#include <stdbool.h>

// {"state":"<state>","bytes":<bytes>,"elapsed_ms":<elapsed_ms>}
// Writes a NUL-terminated string into out (capacity cap); returns its length,
// never exceeding cap-1 (truncates rather than overflow).
int uartrx_rec_state_json(char *out, int cap, const char *state,
                          unsigned bytes, long long elapsed_ms);

// {"fw":"<fw>","dev":"<dev>","baud":<baud>,"gpio":<gpio>,"time_synced":<bool>}
int uartrx_rec_meta_json(char *out, int cap, const char *fw, const char *dev,
                         int baud, int gpio, bool time_synced);
