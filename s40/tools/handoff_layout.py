#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Record symbol offsets in the compiled ARM926 handoff."""
import json
import sys
from pathlib import Path

build = Path(sys.argv[1])
symbols = {row[2]: int(row[0], 16)
           for line in (build / "enter.symbols.txt").read_text().splitlines()
           if len(row := line.split()) == 3}
size = len((build / "enter.bin").read_bytes())
assert symbols["arm926_enter"] == 0
assert 0 < symbols["arm926_identity"] < size <= 4096
assert symbols["arm926_relocate"] < symbols["arm926_relocate_identity"] < size
layout = {"format": 1, "bytes": size, "entry_offset": 0,
          "identity_offset": symbols["arm926_identity"],
          "relocate_offset": symbols["arm926_relocate"],
          "relocate_identity_offset": symbols["arm926_relocate_identity"],
          "relocate_payload_offset": symbols["arm926_relocate_payload"],
          "image_hash_offset": symbols["arm926_image_hash"],
          "marker_offset": symbols["rm443_boot_marker_phys"],
          "marker_call_offset": symbols["rm443_boot_marker_call"],
          "status": "embedded"}
(build / "layout.json").write_text(json.dumps(layout, indent=2) + "\n")
print(f"Built {size}-byte ARM926 handoff with full-DDR relocation")
