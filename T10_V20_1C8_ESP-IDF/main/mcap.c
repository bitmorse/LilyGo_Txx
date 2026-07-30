#include "mcap.h"

#include <assert.h>
#include <string.h>

// MCAP record opcodes we use.
#define OP_HEADER   0x01
#define OP_FOOTER   0x02
#define OP_SCHEMA   0x03
#define OP_CHANNEL  0x04
#define OP_MESSAGE  0x05
#define OP_DATA_END 0x0F

static const uint8_t MCAP_MAGIC[8] = { 0x89, 'M', 'C', 'A', 'P', '0', 0x0D, 0x0A };

// --- little-endian primitives (all MCAP integers are LE) --------------------

static void wbytes(mcap_writer_t *w, const void *p, size_t n)
{
    if (!w->ok) return;
    if (n && fwrite(p, 1, n, w->f) != n) w->ok = false;
}

static void w_u8(mcap_writer_t *w, uint8_t v)  { wbytes(w, &v, 1); }

static void w_u16(mcap_writer_t *w, uint16_t v)
{
    uint8_t b[2] = { v, v >> 8 };
    wbytes(w, b, 2);
}

static void w_u32(mcap_writer_t *w, uint32_t v)
{
    uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 };
    wbytes(w, b, 4);
}

static void w_u64(mcap_writer_t *w, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    wbytes(w, b, 8);
}

// Length-prefixed UTF-8 string (u32 length + bytes).
static void w_str(mcap_writer_t *w, const char *s)
{
    uint32_t n = (uint32_t)strlen(s);
    w_u32(w, n);
    wbytes(w, s, n);
}

static uint32_t str_len(const char *s) { return 4 + (uint32_t)strlen(s); }

// Record framing: opcode + u64 content length. The caller then streams exactly
// `content_len` bytes of fields.
static void rec_hdr(mcap_writer_t *w, uint8_t op, uint64_t content_len)
{
    w_u8(w, op);
    w_u64(w, content_len);
}

// --- public -----------------------------------------------------------------

bool mcap_begin(mcap_writer_t *w, FILE *f)
{
    assert(w != NULL && f != NULL);
    w->f = f;
    for (int i = 0; i < MCAP_MAX_CHANNELS; i++) w->seq[i] = 0;
    w->ok = true;

    wbytes(w, MCAP_MAGIC, sizeof(MCAP_MAGIC));

    // Header: profile (string) + library (string).
    const char *profile = "";               // no well-known profile
    const char *library = "t10-viblog";
    rec_hdr(w, OP_HEADER, str_len(profile) + str_len(library));
    w_str(w, profile);
    w_str(w, library);
    return w->ok;
}

bool mcap_add_schema(mcap_writer_t *w, uint16_t id, const char *name,
                     const char *encoding, const uint8_t *data, uint32_t len)
{
    assert(w != NULL && name && encoding && (data != NULL || len == 0));
    // Schema: id + name + encoding + data (u32 len + bytes).
    rec_hdr(w, OP_SCHEMA, 2 + str_len(name) + str_len(encoding) + 4 + len);
    w_u16(w, id);
    w_str(w, name);
    w_str(w, encoding);
    w_u32(w, len);
    wbytes(w, data, len);
    return w->ok;
}

bool mcap_add_channel(mcap_writer_t *w, uint16_t chan_id, uint16_t schema_id,
                      const char *topic, const char *msg_encoding)
{
    assert(w != NULL && topic && msg_encoding);
    assert(chan_id > 0 && chan_id < MCAP_MAX_CHANNELS);
    // Channel: id + schema_id + topic + message_encoding + metadata(empty map).
    rec_hdr(w, OP_CHANNEL, 2 + 2 + str_len(topic) + str_len(msg_encoding) + 4);
    w_u16(w, chan_id);
    w_u16(w, schema_id);
    w_str(w, topic);
    w_str(w, msg_encoding);
    w_u32(w, 0);                             // metadata map: 0 bytes
    return w->ok;
}

bool mcap_write_message(mcap_writer_t *w, uint16_t chan_id,
                        uint64_t log_ns, uint64_t pub_ns,
                        const uint8_t *data, uint32_t len)
{
    assert(w != NULL && (data != NULL || len == 0));
    assert(chan_id > 0 && chan_id < MCAP_MAX_CHANNELS);
    // Message: channel_id(2) + sequence(4) + log_time(8) + publish_time(8) + data.
    rec_hdr(w, OP_MESSAGE, (uint64_t)22 + len);
    w_u16(w, chan_id);
    w_u32(w, w->seq[chan_id]++);
    w_u64(w, log_ns);
    w_u64(w, pub_ns);
    wbytes(w, data, len);
    return w->ok;
}

bool mcap_close(mcap_writer_t *w)
{
    assert(w != NULL && w->f != NULL);
    // DataEnd: data_section_crc (u32, 0 = not computed).
    rec_hdr(w, OP_DATA_END, 4);
    w_u32(w, 0);

    // Footer: summary_start(8) + summary_offset_start(8) + summary_crc(4), all 0
    // (no summary section).
    rec_hdr(w, OP_FOOTER, 20);
    w_u64(w, 0);
    w_u64(w, 0);
    w_u32(w, 0);

    wbytes(w, MCAP_MAGIC, sizeof(MCAP_MAGIC));
    return w->ok;
}
