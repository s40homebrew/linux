# SPDX-License-Identifier: MIT
"""Reproduce the physical handset's masked SETUP interrupt after USB reset.

The 0.7.5 recording has DEVSTAT=0003, IRQ_EN=0008 and IRQ_SRC=0004/0084:
SETUP is latched but EP0 interrupts are disabled. Exercise the actual state
handler with that register state; the emulator before 0.7.6 retained IRQ_EN.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import function

DRIVER = Path(__file__).resolve().parents[1] / "kernel/overlay/arch/arm/mach-nokia-raps/rm443_usb.c"


class UsbReset(unittest.TestCase):
    def test_reset_rearms_pending_setup_without_clearing_it(self):
        source = DRIVER.read_text()
        handler = function(source, "handle_state") + "\n" + function(source, "rm_pending_irq")
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define BIT(n) (1U << (n))
#define USB_SPEED_UNKNOWN 0
#define USB_SPEED_FULL 2
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (0)
typedef uint16_t u16;
#include "rm443_usb.h"
struct usb_gadget { unsigned speed; };
struct usb_gadget_driver {
    void (*suspend)(struct usb_gadget *);
    void (*resume)(struct usb_gadget *);
};
struct rm_usb {
    struct usb_gadget gadget;
    struct usb_gadget_driver *driver;
    unsigned resets, lock;
    u16 devstat;
    bool callbacks, fault, removing, softconnect, vbus;
};
static u16 devstat, irq_enable, irq_source;
static unsigned reset_callbacks, disconnect_callbacks;
static u16 rm_read(struct rm_usb *usb, unsigned reg) {
    (void)usb;
    if (reg == RM_USB_DEVSTAT) return devstat;
    if (reg == RM_USB_IRQ_EN) return irq_enable;
    if (reg == RM_USB_IRQ_SRC) return irq_source;
    return 0;
}
static void rm_write(struct rm_usb *usb, unsigned reg, u16 value) {
    (void)usb;
    if (reg == RM_USB_IRQ_EN) irq_enable = value;
    if (reg == RM_USB_IRQ_SRC) irq_source &= ~value; /* W1C */
}
static void disconnect_locked(struct rm_usb *usb, bool reset) {
    usb->gadget.speed = USB_SPEED_UNKNOWN;
    if (reset) ++reset_callbacks; else ++disconnect_callbacks;
}
static void spin_unlock(unsigned *lock) { (void)lock; }
static void spin_lock(unsigned *lock) { (void)lock; }
static bool ep0_irq_asserted(void) {
    /* IRQ_EN bit0 enables all three EP0 source bits, not just TX. */
    return (irq_enable & 1) && (irq_source & 7);
}
'''
        main = r'''
int main(void) {
    struct usb_gadget_driver driver = {0};
    struct rm_usb usb = { .driver = &driver, .callbacks = true,
        .softconnect = true, .vbus = true };
    /* Pending raw SOF/DMA cannot spin or fault a PIO-only handler. */
    irq_source = 0x84;
    irq_enable = 8;
    CHECK(rm_pending_irq(&usb) == 0);
    irq_enable = RM_USB_IRQ_ENABLE;
    CHECK(rm_pending_irq(&usb) == RM_USB_IRQ_SETUP);
    irq_source = RM_USB_IRQ_MASK;
    CHECK(rm_pending_irq(&usb) == 0x3f);
    irq_enable = 1;
    CHECK(rm_pending_irq(&usb) == 7);
    irq_enable = 0;
    CHECK(rm_pending_irq(&usb) == 0);
    for (unsigned pass = 0; pass < 3; ++pass) {
        devstat = RM_USB_ATTACHED | RM_USB_RESET;
        /* First reproduce physical mask loss; also test driver-side quiesce. */
        irq_enable = pass ? RM_USB_IRQ_ENABLE : RM_USB_IRQ_STATE;
        irq_source = RM_USB_IRQ_STATE;
        handle_state(&usb);
        CHECK(usb.resets == pass + 1 && reset_callbacks == pass + 1);
        CHECK(usb.gadget.speed == USB_SPEED_UNKNOWN);
        CHECK(irq_enable == RM_USB_IRQ_STATE);

        /* Host has already sent SETUP when reset-end is serviced. */
        devstat = RM_USB_ATTACHED | 2; /* Default address state, physical 0003. */
        irq_source = RM_USB_IRQ_STATE | RM_USB_IRQ_SETUP | BIT(7);
        CHECK(!ep0_irq_asserted());
        handle_state(&usb);
        CHECK(irq_enable == RM_USB_IRQ_ENABLE);
        CHECK(irq_source == (RM_USB_IRQ_SETUP | BIT(7)));
        CHECK(ep0_irq_asserted());
        CHECK(usb.gadget.speed == USB_SPEED_FULL);
    }
    /* A fault or stopped gadget must never be revived by a late state IRQ. */
    for (unsigned guard = 0; guard < 4; ++guard) {
        usb.driver = guard == 0 ? NULL : &driver;
        usb.fault = guard == 1;
        usb.removing = guard == 3;
        usb.devstat = RM_USB_ATTACHED | RM_USB_RESET;
        devstat = guard == 2 ? 0 : RM_USB_ATTACHED | 2;
        irq_enable = 0;
        irq_source = RM_USB_IRQ_STATE | RM_USB_IRQ_SETUP;
        handle_state(&usb);
        CHECK(irq_enable == (guard == 2 ? RM_USB_IRQ_STATE : 0));
        CHECK(irq_source == RM_USB_IRQ_SETUP);
        CHECK(!ep0_irq_asserted());
    }
    CHECK(disconnect_callbacks == 1);
    /* Callback gating is independent of the UDC's bound/running state. */
    usb.driver = &driver; usb.fault = usb.removing = usb.callbacks = false;
    usb.devstat = RM_USB_ATTACHED | RM_USB_RESET;
    devstat = RM_USB_ATTACHED | 2;
    irq_enable = RM_USB_IRQ_STATE;
    irq_source = RM_USB_IRQ_STATE | RM_USB_IRQ_SETUP;
    handle_state(&usb);
    CHECK(irq_enable == RM_USB_IRQ_ENABLE && ep0_irq_asserted());
    puts("reset-end restores EP0 IRQs, preserves SETUP, and respects stop/fault/detach");
    return 0;
}
'''
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="rm443-usb-reset-") as temporary:
            path = Path(temporary)
            (path / "reset.c").write_text(harness + handler + "\n" + main)
            subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(DRIVER.parent), str(path / "reset.c"),
                            "-o", str(path / "reset")], check=True)
            subprocess.run([str(path / "reset")], check=True)


if __name__ == "__main__":
    unittest.main()
