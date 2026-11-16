# SPDX-License-Identifier: MIT
"""Exercise the driver's real PIO helpers against a counted-byte LCD model."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from support import TYPES, UNALIGNED

DRIVER = Path(__file__).resolve().parents[1] / 'kernel/overlay/drivers/gpu/drm/raps/raps_lcd.c'


class LcdTransfer(unittest.TestCase):
    def test_formats_window_color_tails_and_timeout(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('C compiler unavailable')
        source = DRIVER.read_text()
        helpers = source[source.index('int raps_lcd_ready('):source.index('static void raps_lcd_dma_done')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include "raps_pixels.h"
#define RAPS_LCD_STATUS 8
#define RAPS_LCD_MODE 4
#define RAPS_LCD_FIFO 16
struct raps_lcd_io {
    void *base;
    bool failed, dma_stuck;
    u32 bus_mode, pio_delay_us;
    u8 pixel_format, pixel_bytes;
};
#define min(a,b) ((a)<(b)?(a):(b))
#define cond_resched() ((void)0)
struct drm_rect { int x1, y1, x2, y2; };
static u32 registers[8];
static unsigned writes, remaining, count;
static unsigned delayed_usecs;
static bool stuck;
static u8 bytes[200];
static u8 commands[200];
static void udelay(unsigned int usecs) { delayed_usecs += usecs; }
#define readl_poll_timeout_atomic(address, value, condition, delay, timeout) \
    ((value = stuck ? 0 : 8), (condition) ? 0 : -ETIMEDOUT)
static void writel(u32 value, void *address) {
    unsigned offset = (uintptr_t)address - (uintptr_t)registers;
    writes++;
    if (offset == 4) {
        assert((value & 0x3ff00000) == 0x12300000);
        assert(!remaining);
        remaining = (value & 0x3ffff) + 1;
        assert(remaining <= 4);
    }
    registers[offset / 4] = value;
    if (offset == 16) {
        while (remaining) {
            assert(count < sizeof(bytes));
            bytes[count] = value;
            commands[count++] = !(registers[1] & 0x40000);
            value >>= 8;
            remaining--;
        }
    }
}
'''
        checks = r'''
int main(void) {
    struct raps_lcd_io lcd = { .base = registers, .bus_mode = 0x12300000 };
    /* Nonzero X and Y above 255 catch offset and high-byte mistakes. */
    struct drm_rect clip = { 7, 258, 10, 260 };
    assert(raps_lcd_set_window(&lcd, &clip) == 0);
    const u8 expected[] = {0x2a,0,7,0,9,0x2b,1,2,1,3,0x2c};
    assert(count == sizeof(expected) && !memcmp(bytes, expected, count));
    for (unsigned i = 0; i < count; i++) assert(commands[i] == (i == 0 || i == 5 || i == 10));
    const u8 pixels[] = {0xf8,0,7,0xe0,0,0x1f};
    count = 0;
    assert(raps_lcd_pio(&lcd, pixels, sizeof(pixels)) == 0);
    const u8 rgb[] = {0xf8,0,7,0xe0,0,0x1f};
    assert(count == sizeof(rgb) && !memcmp(bytes, rgb, count));
    const u8 formats[] = {0x55, 0x06, 0x07};
    lcd.pio_delay_us = 7;
    for (unsigned i = 0; i < sizeof(formats); ++i) {
        count = delayed_usecs = 0;
        assert(raps_lcd_set_format(&lcd, formats[i]) == 0);
        assert(count == 2 && bytes[0] == 0x3a && bytes[1] == formats[i]);
        assert(commands[0] && !commands[1]);
        assert(delayed_usecs >= lcd.pio_delay_us);
        assert(lcd.pixel_format == formats[i]);
        assert(lcd.pixel_bytes == (formats[i] == 0x55 ? 2 : 3));
    }
    unsigned before = writes;
    assert(raps_lcd_set_format(&lcd, 0xff) == -EINVAL);
    assert(writes == before && lcd.pixel_format == 0x07 && lcd.pixel_bytes == 3);
    const u8 rgb888[] = {0xff,0,0, 0,0xff,0, 0,0,0xff};
    for (unsigned size = 1; size <= sizeof(rgb888); ++size) {
        count = delayed_usecs = 0;
        assert(raps_lcd_pio(&lcd, rgb888, size) == 0);
        assert(count == size && !memcmp(bytes, rgb888, size));
        assert(delayed_usecs >= lcd.pio_delay_us);
        for (unsigned i = 0; i < count; ++i) assert(!commands[i]);
    }
    assert(raps_lcd_ready(&lcd) == 0);
    before = writes;
    stuck = true;
    assert(raps_lcd_pio(&lcd, pixels, sizeof(pixels)) == -ETIMEDOUT);
    assert(lcd.failed && writes == before);
    stuck = false;
    assert(raps_lcd_pio(&lcd, pixels, sizeof(pixels)) == -EIO && writes == before);
    puts("LCD formats, window, RGB wire order, PIO tails, data delays and timeout latch passed");
}
'''
        with tempfile.TemporaryDirectory(prefix='raps-lcd-test-') as temporary:
            path = Path(temporary)
            (path / 'linux').mkdir()
            (path / 'linux/types.h').write_text(TYPES)
            (path / 'linux/unaligned.h').write_text(UNALIGNED)
            (path / 'lcd.c').write_text(harness + helpers + checks)
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(path), '-I', str(DRIVER.parent),
                            str(path / 'lcd.c'), '-o', str(path / 'lcd')], check=True)
            subprocess.run([str(path / 'lcd')], check=True)


if __name__ == '__main__':
    unittest.main()
