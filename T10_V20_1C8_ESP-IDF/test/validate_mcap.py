#!/usr/bin/env python3
"""Validate a vibration-log MCAP against the official mcap + protobuf libraries.

Confirms the file parses as MCAP, the embedded protobuf FileDescriptorSet decodes,
and every AccelBatch message round-trips with the expected shape. Exit 0 on success.
"""
import sys

from mcap.reader import make_reader
from google.protobuf.descriptor_pb2 import FileDescriptorSet
from google.protobuf import descriptor_pool, message_factory

RATE = 4000
N = 500


def main(path):
    with open(path, "rb") as f:
        reader = make_reader(f)
        pool = descriptor_pool.DescriptorPool()
        cls = None
        name = None
        count = 0
        prev_t0 = None
        for schema, channel, message in reader.iter_messages():
            if cls is None:
                assert channel.topic == "/accel", channel.topic
                assert schema.encoding == "protobuf", schema.encoding
                for fd in FileDescriptorSet.FromString(schema.data).file:
                    pool.Add(fd)
                name = schema.name
                cls = message_factory.GetMessageClass(pool.FindMessageTypeByName(name))
            m = cls.FromString(message.data)
            assert len(m.x) == N and len(m.y) == N and len(m.z) == N, "batch size"
            assert m.rate_hz == RATE, m.rate_hz
            assert m.t0_ns == message.log_time, "t0 vs log_time mismatch"
            if prev_t0 is not None:
                # batches are spaced N/RATE seconds apart
                assert m.t0_ns - prev_t0 == N * 1_000_000_000 // RATE, "spacing"
            prev_t0 = m.t0_ns
            count += 1

    assert cls is not None, "no messages found"
    print(f"OK: {path} valid MCAP, schema '{name}', {count} AccelBatch messages decoded")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/viblog_test.mcap"))
