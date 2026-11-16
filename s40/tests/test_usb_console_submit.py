# SPDX-License-Identifier: MIT
"""Exercise a reset interrupt at the real console submission lock boundary."""
import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from support import function

PROJECT = Path(__file__).resolve().parents[1]
from paths import KERNEL_ROOT as UPSTREAM
RELATIVE = "drivers/usb/gadget/function/u_serial.c"
SPEC = importlib.util.spec_from_file_location("kernel_prepare", PROJECT / "kernel/prepare.py")
PREPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARE)

HARNESS = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define GFP_ATOMIC 0
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
struct work_struct { int unused; };
struct usb_request { size_t length; char *buf; int freed, queued; };
struct usb_ep { unsigned maxpacket; };
struct gs_console {
    struct usb_request *req;
    struct { void *data; } console;
    struct work_struct work;
    size_t missed;
    int lock, buf;
};
struct gs_port { struct gs_console *console; };
static struct gs_console console;
static struct gs_port port;
static int irq_enabled, reset_pending, queue_error, stale_submit, dequeued, freed;
static void reset_interrupt(void);
static void maybe_interrupt(void) {
    if (irq_enabled && reset_pending) {
        reset_pending = 0;
        irq_enabled = 0;
        reset_interrupt();
        irq_enabled = 1;
    }
}
static void spin_lock(int *lock) { assert(!*lock); *lock = 1; }
static void spin_unlock(int *lock) { assert(*lock); *lock = 0; }
static void spin_lock_irq(int *lock) { irq_enabled = 0; spin_lock(lock); }
static void spin_unlock_irq(int *lock) {
    spin_unlock(lock); irq_enabled = 1; maybe_interrupt();
}
static size_t kfifo_out(int *buf, char *out, size_t capacity) {
    (void)capacity;
    if (!*buf) return 0;
    *buf = 0; memcpy(out, "boot", 4); return 4;
}
static size_t kfifo_in(int *buf, const char *in, size_t count) {
    (void)buf; (void)in; return count;
}
static int usb_ep_queue(struct usb_ep *ep, struct usb_request *req, int flags) {
    (void)ep; (void)flags;
    assert(!console.lock);
    maybe_interrupt();
    if (req->freed) { stale_submit++; return -1; }
    if (queue_error) return -1;
    req->queued = 1; return 0;
}
static int usb_ep_dequeue(struct usb_ep *ep, struct usb_request *req) {
    (void)ep;
    assert(!console.lock && !req->freed);
    if (!req->queued) return -1;
    req->queued = 0; dequeued++; return 0;
}
static void gs_free_req(struct usb_ep *ep, struct usb_request *req) {
    (void)ep;
    assert(!req->queued && !req->freed);
    /* Mark rather than release memory, so the old race is deterministic. */
    req->freed = 1; freed++;
}
'''

MAIN = r'''
static void reset_interrupt(void) { gs_console_disconnect(&port); }
int main(void) {
    struct usb_ep ep = { .maxpacket = 64 };
    char bytes[64];
    for (int reset = 0; reset <= 1; reset++) {
        for (int error = 0; error <= 1; error++) {
            struct usb_request req = { .buf = bytes };
            console = (struct gs_console){ .req = &req, .buf = 1 };
            console.console.data = &ep; port.console = &console;
            irq_enabled = 1; reset_pending = reset; queue_error = error;
            stale_submit = dequeued = freed = 0;
            gs_console_work(&console.work);
            assert(irq_enabled && !console.lock);
            assert(stale_submit == (reset && !FIXED));
            if (reset) {
                assert(console.req == NULL && freed == 1);
                assert(dequeued == (FIXED && !error));
            } else {
                assert(console.req == &req && !freed);
                assert(req.queued == !error);
                assert(req.length == (error ? 0 : 4));
            }
        }
    }
    puts(FIXED ? "reset waits for submission; normal/error paths preserve lifetime" :
                 "original reset window reproduces stale request submission");
    return 0;
}
'''


class UsbConsoleSubmit(unittest.TestCase):
    def test_reset_submission_and_failure_paths(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="rm443-console-submit-") as directory:
            root = Path(directory)
            path = root / RELATIVE
            path.parent.mkdir(parents=True)
            shutil.copyfile(UPSTREAM / RELATIVE, path)
            original = path.read_text()
            PREPARE.protect_usb_console_submit(root)
            patched = path.read_text()
            timestamp = path.stat().st_mtime_ns
            PREPARE.protect_usb_console_submit(root)
            self.assertEqual(path.read_text(), patched)
            self.assertEqual(path.stat().st_mtime_ns, timestamp)
            for name, text, defines, fixed in (
                ("original", original, [], 0),
                ("raps", patched, ["CONFIG_ARCH_NOKIA_RAPS"], 1),
                ("other", patched, [], 0),
                ("smp", patched, ["CONFIG_ARCH_NOKIA_RAPS", "CONFIG_SMP"], 0),
            ):
                with self.subTest(mode=name):
                    bodies = "\n".join(function(text, f) for f in (
                        "__gs_console_push", "gs_console_work", "gs_console_disconnect"))
                    source, binary = root / (name + ".c"), root / name
                    source.write_text(HARNESS + bodies + MAIN)
                    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    *["-D" + d for d in defines], "-DFIXED=" + str(fixed),
                                    str(source), "-o", str(binary)], check=True)
                    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
