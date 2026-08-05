#!/usr/bin/env python3
"""Validate a UART-RX MCAP against the official `mcap` library.

Confirms the file parses, the channels are schemaless (schema_id 0 -> schema None),
the /uart_rx bytes reassemble byte-exact, and the /state,/meta JSON messages parse.
Exit 0 on success.
"""
import json
import sys

from mcap.reader import make_reader

EXPECTED_RAW = bytes([0x00, 0x01, 0xDE, 0xAD, 0xBE, 0xEF, 0xFF, 0x10, 0x20, 0x00, 0x30])


def main(path):
    topics = {}
    raw = bytearray()
    with open(path, "rb") as f:
        for schema, channel, message in make_reader(f).iter_messages():
            assert schema is None, f"expected schemaless channel, got {schema}"
            topics[channel.topic] = topics.get(channel.topic, 0) + 1
            if channel.topic == "/uart_rx":
                assert channel.message_encoding == "application/octet-stream", channel.message_encoding
                raw += message.data                      # concatenate in log-time order
            else:
                assert channel.message_encoding == "json", channel.message_encoding
                json.loads(message.data)                 # must be valid JSON

    assert topics.get("/uart_rx", 0) >= 2, topics
    assert topics.get("/state", 0) >= 2, topics
    assert topics.get("/meta", 0) >= 1, topics
    assert bytes(raw) == EXPECTED_RAW, bytes(raw).hex()
    print(f"OK: {path} valid MCAP (schemaless), topics {topics}, raw {bytes(raw).hex()}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/uart_test.mcap"))
