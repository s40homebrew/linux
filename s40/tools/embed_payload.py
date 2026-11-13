#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed already-built artifacts, keeping kernel compilation out of JAR rebuilds."""
import json
from pathlib import Path

project = Path(__file__).resolve().parents[1]
build = project / "build/boot_probe"
build.mkdir(parents=True, exist_ok=True)
layout = json.loads((project / "kernel/build/image-layout.json").read_text())
handoff = json.loads((project / "build/handoff/layout.json").read_text())
image = project / "kernel/build/Image"
stub = project / "build/handoff/enter.bin"
if image.stat().st_size != layout["image_bytes"] or stub.stat().st_size != handoff["bytes"]:
    raise SystemExit("stale payload layout metadata; rebuild the corresponding artifact")
image_hash = 2166136261
for value in image.read_bytes():
    image_hash = ((image_hash ^ value) * 16777619) & 0xffffffff
def write_changed(path, text):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
write_changed(build / "payload_layout.h", f'''/* SPDX-License-Identifier: MIT */
#define LINUX_IMAGE_BYTES {layout["image_bytes"]}u
#define LINUX_MEMORY_BYTES {layout["kernel_memory_bytes"]}u
#define LINUX_IMAGE_HASH 0x{image_hash:08x}u
#define HANDOFF_BYTES {handoff["bytes"]}u
#define HANDOFF_IDENTITY_OFFSET {handoff["identity_offset"]}u
#define HANDOFF_RELOCATE_OFFSET {handoff["relocate_offset"]}u
#define HANDOFF_RELOCATE_IDENTITY_OFFSET {handoff["relocate_identity_offset"]}u
#define HANDOFF_MARKER_CALL_OFFSET {handoff["marker_call_offset"]}u
''')
write_changed(build / "payload.S", f'''/* SPDX-License-Identifier: GPL-2.0-or-later */
.section .rodata.linux_image,"a",%progbits
.balign 4
.global linux_image_start, linux_image_end
linux_image_start:
.incbin {json.dumps(str(image))}
linux_image_end:
.section .rodata.handoff,"a",%progbits
.balign 4
.global handoff_start, handoff_end
handoff_start:
.incbin {json.dumps(str(stub))}
handoff_end:
''')
