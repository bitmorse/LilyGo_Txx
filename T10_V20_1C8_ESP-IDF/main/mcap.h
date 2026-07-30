// Minimal streaming MCAP writer (https://mcap.dev).
//
// MCAP is an append-only container: <Magic><Header><records...><DataEnd><Footer>
// <Magic>. We write an unchunked, unindexed file with a single Schema + Channel
// and a stream of Message records. That is a fully valid MCAP that Foxglove Studio
// reads by linear scan -- no summary section, no back-seeking, so it stays
// crash-safe even if power is pulled mid-log. There is no MCAP C library; this is
// the whole format we need, hand-written.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE    *f;
    uint32_t seq;     // message sequence counter
    bool     ok;      // sticky: cleared on the first write error
} mcap_writer_t;

// Begin a file: writes the magic, Header, one Schema and one Channel (id 1).
// `schema_data` is the payload for Schema.encoding (here a serialized protobuf
// FileDescriptorSet). Returns false on any I/O error.
bool mcap_open(mcap_writer_t *w, FILE *f,
               const char *topic, const char *msg_encoding,
               const char *schema_name, const char *schema_encoding,
               const uint8_t *schema_data, uint32_t schema_len);

// Append one Message on the channel. log_ns/pub_ns are UTC nanoseconds.
bool mcap_write_message(mcap_writer_t *w, uint64_t log_ns, uint64_t pub_ns,
                        const uint8_t *data, uint32_t len);

// Write DataEnd + Footer + trailing magic. Does NOT fclose f (caller owns it).
bool mcap_close(mcap_writer_t *w);
