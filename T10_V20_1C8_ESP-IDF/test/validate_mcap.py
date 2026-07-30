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
    pool = descriptor_pool.DescriptorPool()
    classes = {}      # schema name -> message class
    counts = {}       # topic -> message count
    with open(path, "rb") as f:
        reader = make_reader(f)
        for schema, channel, message in reader.iter_messages():
            assert schema.encoding == "protobuf", schema.encoding
            if schema.name not in classes:
                for fd in FileDescriptorSet.FromString(schema.data).file:
                    try:
                        pool.Add(fd)
                    except Exception:
                        pass  # file already added by another schema (shared FDS)
                classes[schema.name] = message_factory.GetMessageClass(
                    pool.FindMessageTypeByName(schema.name))
            m = classes[schema.name].FromString(message.data)
            assert m.t0_ns == message.log_time, "t0 vs log_time mismatch"
            if channel.topic == "/accel":
                assert m.rate_hz == RATE and len(m.x) == N, "accel batch"
                assert len(m.x) == len(m.y) == len(m.z), "accel xyz lengths"
            elif channel.topic == "/imu":
                assert len(m.gx) == len(m.mx) == len(m.temp_c), "imu lengths"
                assert m.rate_hz == 100, m.rate_hz
            counts[channel.topic] = counts.get(channel.topic, 0) + 1

    assert "/accel" in counts, "no /accel messages"
    assert "/imu" in counts, "no /imu messages"
    print(f"OK: {path} valid MCAP, schemas {sorted(classes)}, "
          f"messages {counts}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/viblog_test.mcap"))
