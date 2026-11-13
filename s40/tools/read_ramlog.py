#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read a captured rm443ram earlycon page (not a live memory reader)."""
import argparse
import struct
import sys
from pathlib import Path

MAGIC = 0x5334304c
CAPACITY = 4096 - 16


def decode(data):
    if len(data) != 4096:
        raise ValueError("expected exactly one 4096-byte ramlog page")
    magic, version, count, capacity = struct.unpack_from("<4I", data)
    if (magic, version, capacity) != (MAGIC, 1, CAPACITY):
        raise ValueError("unknown or uninitialized rm443ram header")
    ring = data[16:]
    if count <= capacity:
        return ring[:count]
    start = count % capacity
    return ring[start:] + ring[:start]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    args = parser.parse_args()
    try:
        sys.stdout.buffer.write(decode(args.dump.read_bytes()))
    except ValueError as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
