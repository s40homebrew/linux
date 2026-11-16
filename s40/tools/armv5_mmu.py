#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode ARMv5 short-descriptor tables from OFFLINE physical-memory dumps.

No phone access, guessed virtual aliases, or implicit reads outside supplied data.
The register-only JAR does not produce table dumps; capture those separately.
"""
import argparse
import json
import struct
from dataclasses import asdict, dataclass
from pathlib import Path


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


class TranslationError(ValueError):
    pass


class PhysicalMemory:
    def __init__(self, segments):
        self.segments = sorted(segments)
        end = 0
        for base, data in self.segments:
            if base < end or base < 0 or base + len(data) > 1 << 32:
                raise ValueError("overlapping or out-of-range physical dump")
            end = base + len(data)

    def word(self, address):
        if address & 3:
            raise TranslationError("unaligned descriptor")
        for base, data in self.segments:
            if base <= address and address + 4 <= base + len(data):
                return struct.unpack_from("<I", data, address - base)[0]
        raise TranslationError(f"missing physical descriptor at {address:#010x}")


@dataclass
class Mapping:
    va: int
    mva: int
    pa: int
    granule: int
    domain: int
    ap: int
    cacheable: bool
    bufferable: bool


def translate(memory, ttbr, va, fcse=0):
    """Translate a single address; AP/domain are reported, not bypassed.

    Requires MMU enabled, little-endian tables and an ARMv5 CPU. No ARMv6
    extended descriptors, supersections or LPAE. AP=0 interpretation also
    depends on the ARM926 control register S/R bits; callers must retain them.
    """
    if not 0 <= va < 1 << 32 or not 0 <= ttbr < 1 << 32:
        raise TranslationError("address outside 32-bit space")
    mva = va | (fcse & 0xfe000000) if va < 0x02000000 else va
    first = memory.word((ttbr & 0xffffc000) + 4 * (mva >> 20))
    kind, domain = first & 3, (first >> 5) & 15
    if kind == 0:
        raise TranslationError(f"L1 fault for {va:#010x}")
    if kind == 2:
        return Mapping(va, mva, (first & 0xfff00000) | (mva & 0xfffff),
                       1 << 20, domain, (first >> 10) & 3,
                       bool(first & 8), bool(first & 4))
    fine = kind == 3
    table = first & (0xfffff000 if fine else 0xfffffc00)
    index = (mva >> (10 if fine else 12)) & (1023 if fine else 255)
    second = memory.word(table + 4 * index)
    kind = second & 3
    if kind == 0:
        raise TranslationError(f"L2 fault for {va:#010x}")
    if kind == 1:
        size, ap_shift = 65536, 4 + 2 * ((mva >> 14) & 3)
    elif kind == 2:
        size, ap_shift = 4096, 4 + 2 * ((mva >> 10) & 3)
    elif fine:
        size, ap_shift = 1024, 4
    else:
        raise TranslationError("tiny page in coarse table is reserved on ARMv5")
    return Mapping(va, mva, (second & ~(size - 1)) | (mva & (size - 1)),
                   size, domain, (second >> ap_shift) & 3,
                   bool(second & 8), bool(second & 4))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=Path)
    parser.add_argument("--segment", action="append", default=[], metavar="PHYS:FILE")
    parser.add_argument("--va", type=number, action="append")
    args = parser.parse_args()
    state = json.loads(args.snapshot.read_text())
    if state.get("format") != 1 or state.get("board") != "nokia,rm443":
        parser.error("expected RM-443 register snapshot version 1")
    if number(state["midr"]) & 0xff0ffff0 != 0x41069260:
        parser.error("snapshot is not ARM926")
    control = number(state["control"])
    if not control & 1 or control & 128:
        parser.error("requires enabled MMU and little-endian tables")
    segments = []
    for spec in args.segment:
        base, path = spec.split(":", 1)
        segments.append((number(base), Path(path).read_bytes()))
    memory = PhysicalMemory(segments)
    addresses = args.va or [number(state[key]) for key in ("pc", "sp", "descriptor_va")]
    result = []
    for va in addresses:
        try:
            entry = asdict(translate(memory, number(state["ttbr"]), va, number(state["fcse"])))
            domain = (number(state["domains"]) >> (2 * entry["domain"])) & 3
            entry["domain_access"] = ("fault", "client", "reserved", "manager")[domain]
            result.append(entry)
        except TranslationError as error:
            result.append({"va": va, "error": str(error)})
    print(json.dumps(result, indent=2))
    return int(any("error" in row for row in result))


if __name__ == "__main__":
    raise SystemExit(main())
