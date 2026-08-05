// Integration harness: write a UART-RX-style MCAP with the *production* units
// (main/mcap.c + main/uartrx_rec.c) so it can be validated against the official
// `mcap` library. Channels: /uart_rx (schemaless raw bytes), /state + /meta (json).
// Usage: integration_uart_write <out.mcap>
#include "mcap.h"
#include "uartrx_rec.h"

#include <stdint.h>

// Two raw chunks including NUL and high bytes -- reassembly must be byte-exact.
static const uint8_t CH1[] = {0x00, 0x01, 0xDE, 0xAD, 0xBE, 0xEF, 0xFF};
static const uint8_t CH2[] = {0x10, 0x20, 0x00, 0x30};

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/tmp/uart_test.mcap";
    FILE *f = fopen(path, "wb");
    if (!f) return 2;

    mcap_writer_t w;
    if (!mcap_begin(&w, f)) return 3;
    mcap_add_channel(&w, 1, 0, "/uart_rx", "application/octet-stream");  // schemaless
    mcap_add_channel(&w, 2, 0, "/state",   "json");
    mcap_add_channel(&w, 3, 0, "/meta",    "json");

    char meta[128];
    int mn = uartrx_rec_meta_json(meta, sizeof(meta), "vtest", "T10_TEST", 9600, 21, false);
    if (!mcap_write_message(&w, 3, 1000, 1000, (const uint8_t *)meta, mn)) return 4;

    char st[80];
    int sn = uartrx_rec_state_json(st, sizeof(st), "WAIT", 0, 0);
    if (!mcap_write_message(&w, 2, 2000, 2000, (const uint8_t *)st, sn)) return 5;

    if (!mcap_write_message(&w, 1, 3000, 3000, CH1, sizeof(CH1))) return 6;
    if (!mcap_write_message(&w, 1, 4000, 4000, CH2, sizeof(CH2))) return 7;

    sn = uartrx_rec_state_json(st, sizeof(st), "DATA", 11, 1000);
    if (!mcap_write_message(&w, 2, 4000, 4000, (const uint8_t *)st, sn)) return 8;

    if (!mcap_close(&w)) return 9;
    fclose(f);
    printf("wrote %s (/uart_rx raw + /state,/meta json)\n", path);
    return 0;
}
