# SPDX-License-Identifier: MIT
"""Check the actual callback-veneer emitter; no ARM instructions are executed."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from support import function

MACH = Path(__file__).resolve().parents[1] / "kernel/overlay/arch/arm/mach-nokia-raps"


class CallbackVeneer(unittest.TestCase):
    def test_argument_preserving_instructions_alignment_and_page_bounds(self):
        source = (MACH / "rm443_secure_wdt.c").read_text()
        emit = function(source, "swd_callback_veneer")
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define SZ_4K 4096U
#define ALIGN(n, a) (((n) + (a) - 1) & ~((a) - 1))
'''
        main = r'''
int main(void) {
    _Alignas(4) u8 guarded[SZ_4K + 16];
    u8 *page = guarded + 8;
    const u32 target = 0x1140c298;
    /* Golden ARMv5 encodings use only PC and a non-flag-setting NOP.
     * In particular neither LDR R3 nor BX R3 may replace the incoming R3. */
    const u8 word_aligned[] = {
        0x78,0x47,0xc0,0x46, 0x04,0xf0,0x1f,0xe5, 0x98,0xc2,0x40,0x11
    };
    const u8 halfword_aligned[] = {
        0xc0,0x46,0x78,0x47,0xc0,0x46, 0x04,0xf0,0x1f,0xe5, 0x98,0xc2,0x40,0x11
    };
    for (u32 offset = 0; offset < SZ_4K; offset += 2) {
        const u8 *expected = offset & 2 ? halfword_aligned : word_aligned;
        u32 bytes = offset & 2 ? sizeof(halfword_aligned) : sizeof(word_aligned);
        memset(guarded, 0xa5, sizeof(guarded));
        int rc = swd_callback_veneer(page, offset, target);
        if (bytes > SZ_4K - offset) {
            assert(rc == -EINVAL);
            for (u32 i = 0; i < sizeof(guarded); i++) assert(guarded[i] == 0xa5);
            continue;
        }
        assert(rc == 0 && !memcmp(page + offset, expected, bytes));
        for (u32 i = 0; i < offset + 8; i++) assert(guarded[i] == 0xa5);
        for (u32 i = offset + bytes + 8; i < sizeof(guarded); i++) assert(guarded[i] == 0xa5);
        /* The BX instruction is word-aligned, PC+4 reaches ARM LDR PC,
         * and that instruction's PC-4 literal is the target word. */
        u32 bx = offset + (offset & 2 ? 2 : 0);
        assert(!(bx & 3));
        assert(*(u16 *)(page + bx) == 0x4778);
        assert(*(u32 *)(page + bx + 4) == 0xe51ff004);
        assert(*(u32 *)(page + bx + 4 + 8 - 4) == target);
    }
    const u32 bad_offsets[] = {1, 3, SZ_4K, UINT32_MAX - 1, UINT32_MAX};
    memset(guarded, 0xa5, sizeof(guarded));
    for (u32 i = 0; i < sizeof(bad_offsets)/sizeof(bad_offsets[0]); i++)
        assert(swd_callback_veneer(page, bad_offsets[i], target) == -EINVAL);
    assert(swd_callback_veneer(page, 0, target | 1) == -EINVAL);
    assert(swd_callback_veneer(page, 0, target | 2) == -EINVAL);
    for (u32 i = 0; i < sizeof(guarded); i++) assert(guarded[i] == 0xa5);
    return 0;
}
'''
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix="rm443-callback-veneer-") as directory:
            directory = Path(directory)
            path, binary = directory / "veneer.c", directory / "veneer"
            path.write_text(harness + emit + main)
            subprocess.run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
