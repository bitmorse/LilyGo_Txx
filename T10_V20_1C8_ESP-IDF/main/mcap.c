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

#define SCHEMA_ID   1
#define CHANNEL_ID  1

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

bool mcap_open(mcap_writer_t *w, FILE *f,
               const char *topic, const char *msg_encoding,
               const char *schema_name, const char *schema_encoding,
               const uint8_t *schema_data, uint32_t schema_len)
{
    assert(w != NULL && f != NULL);
    assert(topic && msg_encoding && schema_name && schema_encoding);
    assert(schema_data != NULL || schema_len == 0);

    w->f = f;
    w->seq = 0;
    w->ok = true;

    wbytes(w, MCAP_MAGIC, sizeof(MCAP_MAGIC));

    // Header: profile (string) + library (string).
    const char *profile = "";               // no well-known profile
    const char *library = "t10-viblog";
    rec_hdr(w, OP_HEADER, str_len(profile) + str_len(library));
    w_str(w, profile);
    w_str(w, library);

    // Schema: id + name + encoding + data (u32 len + bytes).
    rec_hdr(w, OP_SCHEMA,
            2 + str_len(schema_name) + str_len(schema_encoding) + 4 + schema_len);
    w_u16(w, SCHEMA_ID);
    w_str(w, schema_name);
    w_str(w, schema_encoding);
    w_u32(w, schema_len);
    wbytes(w, schema_data, schema_len);

    // Channel: id + schema_id + topic + message_encoding + metadata(empty map).
    rec_hdr(w, OP_CHANNEL,
            2 + 2 + str_len(topic) + str_len(msg_encoding) + 4);
    w_u16(w, CHANNEL_ID);
    w_u16(w, SCHEMA_ID);
    w_str(w, topic);
    w_str(w, msg_encoding);
    w_u32(w, 0);                             // metadata map: 0 bytes

    return w->ok;
}

bool mcap_write_message(mcap_writer_t *w, uint64_t log_ns, uint64_t pub_ns,
                        const uint8_t *data, uint32_t len)
{
    assert(w != NULL && (data != NULL || len == 0));
    // Message: channel_id(2) + sequence(4) + log_time(8) + publish_time(8) + data.
    rec_hdr(w, OP_MESSAGE, (uint64_t)22 + len);
    w_u16(w, CHANNEL_ID);
    w_u32(w, w->seq++);
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
