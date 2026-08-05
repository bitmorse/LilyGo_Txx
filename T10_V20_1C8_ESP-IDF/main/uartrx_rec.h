// Pure JSON line builders for the UART RX MCAP recording (the /uart_rx, /state
// and /meta channels). No ESP-IDF deps, so they're unit-tested on the host.
// The /state and /meta inputs are trusted, simple values (fixed state names,
// hex device id, numeric fields), so no JSON string-escaping is done. The
// /uart_rx bytes are arbitrary (possibly binary), so they are base64-encoded
// into a JSON string -- lossless and free of any characters that need escaping.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// {"state":"<state>","bytes":<bytes>,"elapsed_ms":<elapsed_ms>}
// Writes a NUL-terminated string into out (capacity cap); returns its length,
// never exceeding cap-1 (truncates rather than overflow).
int uartrx_rec_state_json(char *out, int cap, const char *state,
                          unsigned bytes, long long elapsed_ms);

// {"fw":"<fw>","dev":"<dev>","baud":<baud>,"gpio":<gpio>,"time_synced":<bool>}
int uartrx_rec_meta_json(char *out, int cap, const char *fw, const char *dev,
                         int baud, int gpio, bool time_synced);

// {"b64":"<standard RFC 4648 base64 of data[0..n)>"} for the /uart_rx channel.
// Writes a NUL-terminated string into out (capacity cap) and returns its length
// (< cap). Returns 0 if the result would not fit -- no partial / invalid-JSON
// output. n must be >= 0. Lossless for arbitrary (including binary) bytes.
int uartrx_rec_uart_b64(char *out, int cap, const uint8_t *data, int n);
