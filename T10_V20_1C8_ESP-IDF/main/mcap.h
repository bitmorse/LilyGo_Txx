// Minimal streaming MCAP writer (https://mcap.dev).
//
// MCAP is an append-only container: <Magic><Header><records...><DataEnd><Footer>
// <Magic>. We write an unchunked, unindexed file with one or more Schema +
// Channel records and a stream of Message records. That is a fully valid MCAP
// that Foxglove Studio reads by linear scan -- no summary section, no
// back-seeking, so it stays crash-safe even if power is pulled mid-log. There is
// no MCAP C library; this is the whole format we need, hand-written.
//
// Usage: mcap_begin -> mcap_add_schema/mcap_add_channel (once each, up front) ->
//        mcap_write_message (repeatedly, any channel) -> mcap_close.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define MCAP_MAX_CHANNELS 8      // channel ids must be 1..MCAP_MAX_CHANNELS-1

typedef struct {
    FILE    *f;
    uint32_t seq[MCAP_MAX_CHANNELS];   // per-channel message sequence counters
    bool     ok;                       // sticky: cleared on the first write error
} mcap_writer_t;

// Write the magic + Header. Call once, first.
bool mcap_begin(mcap_writer_t *w, FILE *f);

// Declare a Schema. `data` is the payload for `encoding` (here a serialized
// protobuf FileDescriptorSet). Call after mcap_begin, before writing messages.
bool mcap_add_schema(mcap_writer_t *w, uint16_t id, const char *name,
                     const char *encoding, const uint8_t *data, uint32_t len);

// Declare a Channel that references a schema. `chan_id` in 1..MCAP_MAX_CHANNELS-1.
bool mcap_add_channel(mcap_writer_t *w, uint16_t chan_id, uint16_t schema_id,
                      const char *topic, const char *msg_encoding);

// Append one Message on `chan_id`. log_ns/pub_ns are UTC nanoseconds.
bool mcap_write_message(mcap_writer_t *w, uint16_t chan_id,
                        uint64_t log_ns, uint64_t pub_ns,
                        const uint8_t *data, uint32_t len);

// Write DataEnd + Footer + trailing magic. Does NOT fclose f (caller owns it).
bool mcap_close(mcap_writer_t *w);
