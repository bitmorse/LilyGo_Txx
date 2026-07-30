// Unit tests for the minimal MCAP writer (main/mcap.c). Writes a file to a
// memory buffer via open_memstream, then walks the records and checks the
// structure against the MCAP spec.
#include "test_framework.h"
#include "mcap.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static const uint8_t MAGIC[8] = { 0x89, 'M', 'C', 'A', 'P', '0', 0x0D, 0x0A };

static uint64_t rd_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static uint32_t rd_u32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }

// Produce a small single-channel MCAP into a heap buffer. Caller frees *buf.
static size_t make_file(uint8_t **buf)
{
    char   *mem = NULL;
    size_t  sz = 0;
    FILE   *f = open_memstream(&mem, &sz);
    uint8_t schema[3] = { 0xAA, 0xBB, 0xCC };

    mcap_writer_t w;
    CHECK(mcap_begin(&w, f));
    CHECK(mcap_add_schema(&w, 1, "vibration.AccelBatch", "protobuf",
                          schema, sizeof(schema)));
    CHECK(mcap_add_channel(&w, 1, 1, "/accel", "protobuf"));
    uint8_t payload[4] = { 1, 2, 3, 4 };
    for (int i = 0; i < 3; i++)
        CHECK(mcap_write_message(&w, 1, 1000 + i, 1000 + i, payload, sizeof(payload)));
    CHECK(mcap_close(&w));
    fclose(f);

    *buf = (uint8_t *)mem;
    return sz;
}

static void test_magic_bookends(void)
{
    uint8_t *b;
    size_t n = make_file(&b);
    CHECK(n > 16);
    CHECK_EQ(memcmp(b, MAGIC, 8), 0);                 // leading magic
    CHECK_EQ(memcmp(b + n - 8, MAGIC, 8), 0);         // trailing magic
    free(b);
}

// Walk records between the two magics, collecting opcodes and message fields.
static void test_record_structure(void)
{
    uint8_t *b;
    size_t n = make_file(&b);

    size_t off = 8;                                   // past leading magic
    size_t end = n - 8;                               // before trailing magic
    int seen[256] = { 0 };
    int msg_count = 0;
    uint32_t last_seq = 0;
    uint64_t first_logtime = 0;

    while (off < end) {
        uint8_t op = b[off];
        uint64_t len = rd_u64(b + off + 1);
        const uint8_t *body = b + off + 9;
        CHECK(off + 9 + len <= n);                    // record stays in bounds
        seen[op]++;
        if (op == 0x05) {                             // Message
            uint16_t chan = rd_u16(body);
            uint32_t seq  = rd_u32(body + 2);
            uint64_t logt = rd_u64(body + 6);
            CHECK_EQ(chan, 1);                        // channel id
            CHECK_EQ(seq, msg_count);                 // sequence 0,1,2...
            if (msg_count == 0) first_logtime = logt;
            last_seq = seq;
            // data length = record length - 22 header bytes
            CHECK_EQ(len - 22, 4);
            msg_count++;
        }
        off += 9 + len;
    }
    CHECK_EQ(off, end);                               // records exactly fill gap
    CHECK_EQ(seen[0x01], 1);                          // one Header
    CHECK_EQ(seen[0x03], 1);                          // one Schema
    CHECK_EQ(seen[0x04], 1);                          // one Channel
    CHECK_EQ(seen[0x05], 3);                          // three Messages
    CHECK_EQ(seen[0x0F], 1);                          // one DataEnd
    CHECK_EQ(seen[0x02], 1);                          // one Footer
    CHECK_EQ(msg_count, 3);
    CHECK_EQ(last_seq, 2);
    CHECK_EQ(first_logtime, 1000);
    free(b);
}

// Header must be the first record and precede Schema/Channel.
static void test_header_first(void)
{
    uint8_t *b;
    size_t n = make_file(&b);
    CHECK_EQ(b[8], 0x01);                             // first record = Header
    free(b);
    (void)n;
}

// Schema record carries the embedded schema bytes intact.
static void test_schema_payload(void)
{
    uint8_t *b;
    size_t n = make_file(&b);
    size_t off = 8, end = n - 8;
    int found = 0;
    while (off < end) {
        uint8_t op = b[off];
        uint64_t len = rd_u64(b + off + 1);
        if (op == 0x03) {                             // Schema
            const uint8_t *p = b + off + 9;
            CHECK_EQ(rd_u16(p), 1);                   // schema id
            uint32_t namelen = rd_u32(p + 2);
            const uint8_t *q = p + 2 + 4 + namelen;   // skip name
            uint32_t enclen = rd_u32(q);
            q += 4 + enclen;                          // skip encoding
            uint32_t datalen = rd_u32(q);
            CHECK_EQ(datalen, 3);                     // our 3 schema bytes
            CHECK_EQ(q[4], 0xAA); CHECK_EQ(q[5], 0xBB); CHECK_EQ(q[6], 0xCC);
            found = 1;
        }
        off += 9 + len;
    }
    CHECK(found);
    free(b);
}

// Two schemas + two channels, messages interleaved: each channel keeps its own
// sequence counter and records carry the right channel id.
static void test_multi_channel(void)
{
    char   *mem = NULL;
    size_t  sz = 0;
    FILE   *f = open_memstream(&mem, &sz);
    uint8_t s1[2] = { 0x11, 0x22 }, s2[2] = { 0x33, 0x44 };

    mcap_writer_t w;
    CHECK(mcap_begin(&w, f));
    CHECK(mcap_add_schema(&w, 1, "vibration.AccelBatch", "protobuf", s1, 2));
    CHECK(mcap_add_schema(&w, 2, "vibration.ImuAux", "protobuf", s2, 2));
    CHECK(mcap_add_channel(&w, 1, 1, "/accel", "protobuf"));
    CHECK(mcap_add_channel(&w, 2, 2, "/imu", "protobuf"));
    uint8_t p[2] = { 9, 9 };
    // /accel gets 3 messages, /imu gets 2, interleaved.
    CHECK(mcap_write_message(&w, 1, 10, 10, p, 2));
    CHECK(mcap_write_message(&w, 2, 11, 11, p, 2));
    CHECK(mcap_write_message(&w, 1, 12, 12, p, 2));
    CHECK(mcap_write_message(&w, 2, 13, 13, p, 2));
    CHECK(mcap_write_message(&w, 1, 14, 14, p, 2));
    CHECK(mcap_close(&w));
    fclose(f);

    uint8_t *b = (uint8_t *)mem;
    size_t off = 8, end = sz - 8;
    int seen[256] = { 0 };
    int seq_by_chan[3] = { 0, 0, 0 };     // expected next sequence per channel
    int msgs_chan[3] = { 0, 0, 0 };
    while (off < end) {
        uint8_t op = b[off];
        uint64_t len = rd_u64(b + off + 1);
        seen[op]++;
        if (op == 0x05) {
            uint16_t chan = rd_u16(b + off + 9);
            uint32_t seq  = rd_u32(b + off + 9 + 2);
            CHECK(chan == 1 || chan == 2);
            CHECK_EQ(seq, seq_by_chan[chan]);     // per-channel sequence
            seq_by_chan[chan]++;
            msgs_chan[chan]++;
        }
        off += 9 + len;
    }
    CHECK_EQ(seen[0x03], 2);              // two schemas
    CHECK_EQ(seen[0x04], 2);              // two channels
    CHECK_EQ(seen[0x05], 5);             // five messages total
    CHECK_EQ(msgs_chan[1], 3);
    CHECK_EQ(msgs_chan[2], 2);
    free(b);
}

int main(void)
{
    printf("test_mcap\n");
    RUN(test_magic_bookends);
    RUN(test_record_structure);
    RUN(test_header_first);
    RUN(test_schema_payload);
    RUN(test_multi_channel);
    return REPORT();
}
