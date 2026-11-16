# SPDX-License-Identifier: MIT
"""Small shared helpers for the host C and ARM checks."""
from functools import cache
import json
import re
import subprocess
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
MACH = PROJECT / "kernel/overlay/arch/arm/mach-nokia-raps"


def function(source, name):
    """Extract a static C definition without counting braces in comments/strings."""
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
    plain = tokens.sub(lambda match: " " * len(match[0]), source)
    start = plain.rfind("\nstatic ", 0, plain.index(name + "(")) + 1
    body = plain.index("{", start)
    depth, end = 1, body + 1
    while depth:
        depth += (plain[end] == "{") - (plain[end] == "}")
        end += 1
    return source[start:end]


@cache
def handoff():
    """Build the shared handoff once per test process."""
    subprocess.run(["bash", str(PROJECT / "build_handoff.sh")],
                   check=True, capture_output=True)
    directory = PROJECT / "build/handoff"
    return (directory / "enter.bin").read_bytes(), json.loads((directory / "layout.json").read_text())


TYPES = """
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
"""

UNALIGNED = """
static inline u16 get_unaligned_be16(const void *source)
{
    const u8 *p = source;
    return (u16)p[0] << 8 | p[1];
}
static inline u32 get_unaligned_le32(const void *source)
{
    const u8 *p = source;
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
"""
