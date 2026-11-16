# SPDX-License-Identifier: MIT
"""Run the UDC's boot wait with host control requests and a simulated clock."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from support import MACH, function

DRIVER = MACH / "rm443_usb.c"


class UsbBootWait(unittest.TestCase):
    def test_dtr_timeout_disconnect_and_watchdogs(self):
        source = DRIVER.read_text()
        helpers = "\n".join(function(source, name) for name in
                            ("rm_track_console_open", "disconnect_locked", "rm_wait_for_console"))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdarg.h>
#include <errno.h>
#define USB_DIR_OUT 0
#define USB_DIR_IN 0x80
#define USB_TYPE_CLASS 0x20
#define USB_RECIP_INTERFACE 1
#define USB_RECIP_DEVICE 0
#define USB_REQ_SET_CONFIGURATION 9
#define USB_CDC_REQ_SET_CONTROL_LINE_STATE 0x22
#define USB_CDC_CTRL_DTR 1
#define USB_SPEED_UNKNOWN 0
#define RM_EP0_IDLE 0
#define RM_EP_COUNT 7
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, new_value) ((value) = (new_value))
#define le16_to_cpu(value) (value)
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define time_before(a, b) ((long)((a) - (b)) < 0)
struct usb_ctrlrequest { uint8_t bRequestType, bRequest; uint16_t wValue, wIndex, wLength; };
struct usb_gadget { unsigned speed; };
struct usb_gadget_driver { void (*disconnect)(struct usb_gadget *); };
struct rm_ep { bool enabled; };
struct rm_usb {
    struct usb_gadget gadget;
    struct usb_gadget_driver *driver;
    void *dev;
    unsigned lock, ep0_state;
    bool console_open, callbacks;
    struct rm_ep eps[RM_EP_COUNT];
};
static unsigned int boot_wait_ms;
static unsigned long jiffies;
static unsigned feeds, feeds_at_sleep, last_sleep_ms, elapsed, open_at;
static unsigned failures_left;
static int last_reported_error;
static struct rm_usb *waiting;
static unsigned long msecs_to_jiffies(unsigned ms) { return ms; }
static int rm443_power_refresh(void) {
    ++feeds;
    if (failures_left) { --failures_left; return -EIO; }
    return 0;
}
static void msleep(unsigned ms) {
    assert(ms > 0 && feeds > feeds_at_sleep);
    feeds_at_sleep = feeds; last_sleep_ms = ms;
    elapsed += ms; jiffies += ms;
    assert(elapsed <= 31000);
    if (open_at && elapsed >= open_at) waiting->console_open = true;
}
static void dev_info(void *dev, const char *format, ...) { (void)dev; (void)format; }
static void dev_err_once(void *dev, const char *format, ...) {
    (void)dev;
    va_list args;
    va_start(args, format);
    last_reported_error = va_arg(args, int);
    va_end(args);
}
static void nuke(struct rm_ep *ep, int status) { (void)ep; assert(status == -ESHUTDOWN); }
static void clear_fifo(struct rm_usb *usb, unsigned address) { (void)usb; (void)address; }
static void spin_unlock(unsigned *lock) { (void)lock; }
static void spin_lock(unsigned *lock) { (void)lock; }
static void usb_gadget_udc_reset(struct usb_gadget *gadget, struct usb_gadget_driver *driver) {
    (void)gadget; (void)driver;
}
'''
        main = r'''
static void run_wait(struct rm_usb *usb, unsigned limit, unsigned attach,
                     unsigned expected, bool opened) {
    feeds = feeds_at_sleep = last_sleep_ms = elapsed = 0;
    open_at = attach; boot_wait_ms = limit; waiting = usb;
    rm_wait_for_console(usb);
    if (expected) assert(elapsed >= expected && elapsed - expected < last_sleep_ms);
    else assert(!elapsed && !feeds);
    assert(usb->console_open == opened);
}
int main(void) {
    struct rm_usb usb = {0};
    struct usb_ctrlrequest setup = { .bRequestType = 0x21, .bRequest = 0x22, .wValue = 1 };
    /* Rejected, wrong-direction, data-bearing and non-ACM requests cannot
     * release boot. Accepted DTR, including DTR+RTS, can; RTS alone cannot. */
    rm_track_console_open(&usb, &setup, -EOPNOTSUPP); assert(!usb.console_open);
    setup.bRequestType = 0xa1;
    rm_track_console_open(&usb, &setup, 0); assert(!usb.console_open);
    setup.bRequestType = 0x21; setup.wLength = 1;
    rm_track_console_open(&usb, &setup, 0); assert(!usb.console_open);
    setup.wLength = 0; setup.bRequest = 0x20;
    rm_track_console_open(&usb, &setup, 0); assert(!usb.console_open);
    setup.bRequest = 0x22;
    rm_track_console_open(&usb, &setup, 0); assert(usb.console_open);
    setup.wValue = 2;
    rm_track_console_open(&usb, &setup, 0); assert(!usb.console_open);
    setup.wValue = 3;
    rm_track_console_open(&usb, &setup, 0); assert(usb.console_open);
    /* Host close, reset, unplug, or unconfigure clears any previous open. */
    setup.wValue = 0;
    rm_track_console_open(&usb, &setup, 0); assert(!usb.console_open);
    for (unsigned reset = 0; reset < 2; ++reset) {
        usb.console_open = true;
        disconnect_locked(&usb, reset); assert(!usb.console_open);
    }
    usb.console_open = true;
    setup.bRequestType = 0; setup.bRequest = 9;
    rm_track_console_open(&usb, &setup, 0); assert(!usb.console_open);
    run_wait(&usb, 0, 0, 0, false);
    run_wait(&usb, 60, 0, 60, false);
    run_wait(&usb, UINT_MAX, 0, 30000, false);
    jiffies = ULONG_MAX - 19;
    run_wait(&usb, 60, 0, 60, false); /* deadline wraps */
    run_wait(&usb, 100, 40, 40, true);
    run_wait(&usb, 100, 0, 0, true); /* already open */
    assert(!last_reported_error);
    usb.console_open = false;
    failures_left = 2;
    run_wait(&usb, 100, 60, 60, true);
    assert(last_reported_error == -EIO);
    return 0;
}
'''
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="rm443-usb-wait-") as temporary:
            path = Path(temporary)
            (path / "wait.c").write_text(harness + helpers + "\n" + main)
            subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            str(path / "wait.c"), "-o", str(path / "wait")], check=True)
            subprocess.run([str(path / "wait")], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
