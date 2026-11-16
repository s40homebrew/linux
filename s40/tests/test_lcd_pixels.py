# SPDX-License-Identifier: MIT
"""Exercise the actual LCD packing helpers, including guarded FIFO tails."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
DRIVER = PROJECT / "kernel/overlay/drivers/gpu/drm/raps"

from support import TYPES, UNALIGNED

CHECK = r"""
#include <assert.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "raps_pixels.h"

int main(void)
{
    static const u16 inputs[] = {0, 0xffff, 0xf800, 0x07e0, 0x001f,
                                0x07ff, 0x8410, 0x4208};
    static const u8 expected[][3] = {{0,0,0}, {255,255,255}, {255,0,0},
        {0,255,0}, {0,0,255}, {0,255,255}, {132,130,132}, {66,65,66}};
    static const u8 packed[] = {0xf8,0, 7,0xe0, 0,0x1f, 0xff,0xff, 0,0, 0x84,0x10};
    static const u8 expanded[] = {255,0,0, 0,255,0, 0,0,255,
                                 255,255,255, 0,0,0, 132,130,132};
    static const u8 bgr[] = {3,2,1, 6,5,4, 9,8,7};
    static const u8 rgb[] = {1,2,3, 4,5,6, 7,8,9};
    static const u32 words[] = {0x12, 0x3412, 0x563412, 0x78563412};
    static const u8 tail[] = {0x12,0x34,0x56,0x78};
    u8 storage[24], pixel[3], order[sizeof(bgr)];
    long page = sysconf(_SC_PAGESIZE);
    u8 *mapping;
    unsigned int i;

    for (i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        raps_pixel_rgb888(pixel, inputs[i]);
        assert(!memcmp(pixel, expected[i], sizeof(pixel)));
    }
    memset(storage, 0xa5, sizeof(storage));
    memcpy(storage + 3, packed, sizeof(packed));
    raps_pixels_expand(storage + 3, 6);
    assert(!memcmp(storage + 3, expanded, sizeof(expanded)));
    for (i = 0; i < 3; ++i) {
        assert(storage[i] == 0xa5);
        assert(storage[21 + i] == 0xa5);
    }
    raps_pixels_expand(storage + 3, 0);
    assert(!memcmp(storage + 3, expanded, sizeof(expanded)));
    memcpy(order, bgr, sizeof(order));
    raps_pixels_rgb_order(order, 3);
    assert(!memcmp(order, rgb, sizeof(rgb)));
    raps_pixels_rgb_order(order, 0);
    assert(!memcmp(order, rgb, sizeof(rgb)));

    assert(page > 0);
    mapping = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    assert(!mprotect(mapping + page, (size_t)page, PROT_NONE));
    for (i = 1; i <= 4; ++i) {
        u8 *end = mapping + page - i;
        memcpy(end, tail, i);
        assert(raps_pixel_word(end, i) == words[i - 1]);
    }
    assert(!munmap(mapping, (size_t)page * 2));
    return 0;
}
"""


class LcdPixels(unittest.TestCase):
    def test_pixel_packing_and_bounded_fifo_tails(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler needed")
        with tempfile.TemporaryDirectory(prefix="rm443-lcd-pixels-") as temporary:
            path = Path(temporary)
            (path / "linux").mkdir()
            (path / "linux/types.h").write_text(TYPES)
            (path / "linux/unaligned.h").write_text(UNALIGNED)
            (path / "pixels.c").write_text(CHECK)
            binary = path / "pixels"
            subprocess.run([compiler, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-I", str(path), "-I", str(DRIVER), str(path / "pixels.c"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
