#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Report the uncompressed Image footprint from matching vmlinux nm output."""
import argparse
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("symbols", type=Path)
    args = parser.parse_args()
    symbols = {}
    for line in args.symbols.read_text().splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in ("_text", "_end", "swapper_pg_dir", "__bss_start"):
            symbols[parts[2]] = int(parts[0], 16)
    if set(symbols) != {"_text", "_end", "swapper_pg_dir", "__bss_start"}:
        parser.error("missing required vmlinux symbols")
    size = args.image.stat().st_size
    memory = symbols["_end"] - symbols["_text"]
    if not 0 < size <= memory or symbols["_text"] & 0xFFFF != 0x8000:
        parser.error("unexpected Image size/layout")
    print(json.dumps({
        "format": "linux-arm-uncompressed-image",
        "cpu": "ARM926EJ-S / ARMv5TE",
        "image_bytes": size,
        "kernel_memory_bytes": memory,
        "text_offset": 0x8000,
        "minimum_bank_prefix_reserved_bytes": 0x8000 + memory,
        "physical_bank_alignment": 0x200000,
        "linked_symbols": {name: hex(value) for name, value in symbols.items()},
    }, indent=2))


if __name__ == "__main__":
    main()
