#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build an offline ARMv5 transition table. Does not reserve or write phone RAM.

One page of handoff code is mapped at its current VA and at VA=PA. All other
pages fault. Inputs must come from a live, still-owned contiguous allocation.
"""
import argparse
import json
import struct
from pathlib import Path

PAGE = 4096
TABLE_BYTES = 16384 + 2 * 1024


def integer(text):
    return int(text, 0)


def checked_range(base, size):
    if not 0 <= base < 1 << 32 or size <= 0 or base + size > 1 << 32:
        raise ValueError("invalid 32-bit memory range")
    return base, base + size


def overlaps(a, b):
    return a[0] < b[1] and b[0] < a[1]


def build_tables(stub_va, stub_pa, table_pa):
    code = checked_range(stub_pa, PAGE)
    checked_range(stub_va, PAGE)
    tables = checked_range(table_pa, TABLE_BYTES)
    if (stub_va | stub_pa) & (PAGE - 1) or table_pa & 16383:
        raise ValueError("stub VA/PA must be page aligned; TTBR must be 16-KiB aligned")
    if overlaps(code, tables):
        raise ValueError("transition code overlaps page tables")
    # FCSE is required to be zero; don't support low code VAs in the first port.
    if stub_va < 0x02000000 or stub_pa < 0x02000000:
        raise ValueError("transition code must be above the FCSE region")
    result = bytearray(TABLE_BYTES)
    coarse_tables = {}
    for va in sorted({stub_va, stub_pa}):
        slot = va >> 20
        if slot not in coarse_tables:
            offset = 16384 + 1024 * len(coarse_tables)
            coarse_tables[slot] = offset
            # ARMv5 coarse table, domain 0, implementation-required bit 4.
            struct.pack_into("<I", result, slot * 4, (table_pa + offset) | 0x11)
        index = (va >> 12) & 255
        # Four AP=1 subpages: privileged R/W, user inaccessible. C=B=0.
        struct.pack_into("<I", result, coarse_tables[slot] + index * 4, stub_pa | 0x552)
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stub-va", required=True, type=integer)
    parser.add_argument("--stub-pa", required=True, type=integer)
    parser.add_argument("--table-pa", required=True, type=integer)
    parser.add_argument("--owned-base", required=True, type=integer)
    parser.add_argument("--owned-bytes", required=True, type=integer)
    parser.add_argument("--layout", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        bank = checked_range(args.owned_base, args.owned_bytes)
        for base, size in ((args.stub_pa, PAGE), (args.table_pa, TABLE_BYTES)):
            region = checked_range(base, size)
            if region[0] < bank[0] or region[1] > bank[1]:
                raise ValueError("transition memory outside caller-supplied owned arena")
        layout = json.loads(args.layout.read_text())
        if layout["format"] != 1 or layout["entry_offset"] != 0:
            raise ValueError("unexpected handoff layout")
        if not 0 < layout["identity_offset"] < layout["bytes"] <= PAGE:
            raise ValueError("handoff does not fit in one page")
        blob = build_tables(args.stub_va, args.stub_pa, args.table_pa)
    except (ValueError, KeyError) as error:
        parser.error(str(error))
    args.out.write_bytes(blob)
    print(json.dumps({"status": "offline-only-ownership-not-verified",
                      "ttbr_pa": args.table_pa, "table_bytes": len(blob),
                      "entry_va": args.stub_va,
                      "identity_pa": args.stub_pa + layout["identity_offset"],
                      "must_reserve": [[args.stub_pa, PAGE], [args.table_pa, len(blob)]],
                      "missing": ["live allocation/VA-to-PA verification", "DMA/modem quiescence",
                                  "watchdog policy", "kernel/DTB/ring nonoverlap"]}, indent=2))


if __name__ == "__main__":
    main()
