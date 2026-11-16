# SPDX-License-Identifier: MIT
"""Check the actual driver's PHY switch ordering, preserved bits and CBUS faults.

The emulator's attach predicate currently ignores Betty's peripheral switch;
this focused host contract test catches the physical-attach omission it missed.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
DRIVER = PROJECT / 'kernel/overlay/arch/arm/mach-nokia-raps/rm443_usb.c'


class UsbAttach(unittest.TestCase):
    def test_betty_switch_and_transaction_failures(self):
        source = DRIVER.read_text()
        function = source[source.index('static int apply_pullup('):source.index('\nstatic int rm_pullup(')]
        constants = '\n'.join(line for line in source.splitlines() if line.startswith('#define RM_USB_BETTY'))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define BIT(n) (1U << (n))
typedef uint16_t u16;
typedef uint32_t u32;
#include "rm443_usb.h"
struct rm_usb {
    void *driver, *dev, *otg, *pinmux;
    bool softconnect, vbus, fault;
    unsigned errors;
};
static u32 otg[8], pins;
static u16 udc[64], betty;
static unsigned events[32], count, calls, fail_call;
static void event(unsigned code) { assert(count < 32); events[count++] = code; }
static u32 readl(void *p) { return *(u32 *)p; }
static void writel(u32 value, void *p) {
    *(u32 *)p = value;
    event(p == &pins ? 20 : 21);
}
static u16 rm_read(struct rm_usb *usb, unsigned reg) { (void)usb; return udc[reg / 2]; }
static void rm_write(struct rm_usb *usb, unsigned reg, u16 value) {
    (void)usb; udc[reg / 2] = value; event(reg);
}
static void udelay(unsigned value) { assert(value == 5); event(30); }
static int rm443_cbus_update(unsigned chip, unsigned reg, u16 mask, u16 value) {
    assert(chip == 2 && reg == 6);
    event(100 + mask);
    if (++calls == fail_call) return -110;
    betty = (betty & ~mask) | (value & mask);
    return 0;
}
#define dev_err_ratelimited(...) ((void)0)
'''
        tests = r'''
static struct rm_usb reset(bool connected, unsigned failure) {
    struct rm_usb usb = { .driver = (void *)1, .dev = (void *)1,
        .otg = otg, .pinmux = &pins, .softconnect = connected, .vbus = true };
    count = calls = 0; fail_call = failure;
    betty = 0xa020 | (connected ? 0 : 6);
    pins = 0x12340000;
    otg[RM_OTG_CTRL / 4] = 0x100000 | (connected ? 0 : RM_OTG_BSESSVLD);
    udc[RM_USB_SYSCON1 / 2] = 0x104 | (connected ? 0 : RM_USB_PULLUP);
    udc[RM_USB_IRQ_EN / 2] = RM_USB_IRQ_ENABLE;
    return usb;
}
int main(void) {
    struct rm_usb usb = reset(true, 0);
    assert(apply_pullup(&usb) == 0);
    assert(betty == 0xa026);
    assert(pins == 0x123400ff);
    assert(otg[RM_OTG_CTRL / 4] == (0x100000 | RM_OTG_BSESSVLD));
    assert(udc[RM_USB_SYSCON1 / 2] == 0x105);
    /* NSUSPEND -> settle -> mux -> session -> controller pullup -> switch. */
    const unsigned attach[] = {102, 30, 20, 21, RM_USB_SYSCON1, 104};
    assert(count == sizeof(attach) / sizeof(attach[0]));
    for (unsigned i = 0; i < count; ++i) assert(events[i] == attach[i]);
    usb = reset(false, 0);
    assert(apply_pullup(&usb) == 0);
    assert(betty == 0xa020 && udc[RM_USB_SYSCON1 / 2] == 0x104);
    const unsigned detach[] = {RM_USB_SYSCON1, 21, 104, 30, 102};
    assert(count == sizeof(detach) / sizeof(detach[0]));
    for (unsigned i = 0; i < count; ++i) assert(events[i] == detach[i]);
    for (unsigned connected = 0; connected < 2; ++connected) {
        for (unsigned failure = 1; failure <= 2; ++failure) {
            usb = reset(connected, failure);
            assert(apply_pullup(&usb) == -110);
            assert(usb.fault && usb.errors == 1);
            assert(!(udc[RM_USB_SYSCON1 / 2] & RM_USB_PULLUP));
            assert(!(otg[RM_OTG_CTRL / 4] & RM_OTG_BSESSVLD));
            assert(udc[RM_USB_IRQ_EN / 2] == 0);
            assert((betty & ~6U) == 0xa020);
        }
    }
    puts("normal attach/detach and four CBUS failure boundaries passed");
}
'''
        with tempfile.TemporaryDirectory(prefix='rm443-usb-attach-') as temporary:
            path = Path(temporary)
            (path / 'attach.c').write_text(harness + '\n' + constants + '\n' + function + '\n' + tests)
            compiler = shutil.which('cc')
            if not compiler:
                self.skipTest('C compiler unavailable')
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                '-I', str(DRIVER.parent), str(path / 'attach.c'), '-o', str(path / 'attach')], check=True)
            subprocess.run([str(path / 'attach')], check=True)


if __name__ == '__main__':
    unittest.main()
