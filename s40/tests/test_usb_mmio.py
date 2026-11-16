# SPDX-License-Identifier: MIT
"""Exercise actual UDC helpers against the firmware's MMIO width contract."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import function

DRIVER = Path(__file__).resolve().parents[1] / "kernel/overlay/arch/arm/mach-nokia-raps/rm443_usb.c"


class UsbMmio(unittest.TestCase):
    def test_word_registers_and_byte_fifo(self):
        source = DRIVER.read_text()
        helpers = "\n".join(function(source, name) for name in
                            ("rm_read", "rm_write", "ep_selector", "select_ep", "write_packet"))
        receive = function(source, "handle_out")
        setup = function(source, "handle_setup")
        setup = setup[setup.index("\trm_write(usb, RM_USB_EP_NUM, RM_USB_SETUP_SEL);"):
                      setup.index("\tvalue = le16_to_cpu")]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#ifndef EREMOTEIO
#define EREMOTEIO 121
#endif
#define BIT(n) (1U << (n))
#define USB_DIR_IN 0x80
#define min(a,b) ((a) < (b) ? (a) : (b))
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#include "rm443_usb.h"
enum { RM_EP0_IDLE, RM_EP0_IN, RM_EP0_OUT, RM_EP0_STATUS_IN, RM_EP0_STATUS_OUT };
struct usb_ctrlrequest { u8 bytes[8]; };
struct rm_usb {
    void *regs;
    unsigned in_packets, out_packets, errors, ep0_state;
    struct usb_ctrlrequest last_setup;
};
struct rm_request {
    struct { void *buf; unsigned length, actual; bool short_not_ok; } req;
};
struct rm_ep {
    struct rm_usb *usb;
    struct { unsigned maxpacket; } ep;
    u8 address;
    bool busy;
};
static u32 regs[64];
static u8 tx[64], rx[64];
static unsigned tx_count, rx_count, rx_cursor, completions;
static int completion_status;
static struct rm_request *pending;
static u32 readl(void *address) {
    unsigned offset = (u8 *)address - (u8 *)regs;
    assert(!(offset & 3) && offset != RM_USB_DATA);
    return regs[offset / 4];
}
static void writel(u32 value, void *address) {
    unsigned offset = (u8 *)address - (u8 *)regs;
    assert(!(offset & 3) && offset != RM_USB_DATA);
    if (offset == RM_USB_CTRL && (value & RM_USB_FIFO_EN))
        assert(!(regs[RM_USB_EP_NUM / 4] & RM_USB_EP_SEL));
    regs[offset / 4] = value;
}
static u8 readb(void *address) {
    assert(address == (u8 *)regs + RM_USB_DATA);
    assert(regs[RM_USB_EP_NUM / 4] & (RM_USB_EP_SEL | RM_USB_SETUP_SEL));
    assert(rx_cursor < rx_count);
    return rx[rx_cursor++];
}
static void writeb(u8 value, void *address) {
    assert(address == (u8 *)regs + RM_USB_DATA);
    assert(regs[RM_USB_EP_NUM / 4] & RM_USB_EP_SEL);
    assert(tx_count < sizeof(tx));
    tx[tx_count++] = value;
}
static struct rm_request *first_request(struct rm_ep *ep) { (void)ep; return pending; }
static void clear_fifo(struct rm_usb *usb, u8 address) { (void)usb; (void)address; }
static void kick(struct rm_ep *ep) { (void)ep; }
static void stall_ep0(struct rm_usb *usb) { (void)usb; }
static void status_in(struct rm_usb *usb) { (void)usb; }
static void giveback(struct rm_ep *ep, struct rm_request *req, int status) {
    (void)ep; assert(req == pending); ++completions; completion_status = status;
}
'''
        main = r'''
int main(void) {
    struct rm_usb usb = { .regs = regs };
    u8 pattern[64], output[66];
    for (unsigned i = 0; i < sizeof(pattern); ++i) pattern[i] = i * 17 + 3;
    /* Every length, including odd tails and zero-length status packets. */
    for (unsigned count = 0; count <= 64; ++count) {
        tx_count = 0;
        write_packet(&usb, USB_DIR_IN, count ? pattern : NULL, count);
        assert(tx_count == count && !memcmp(tx, pattern, count));
        assert(regs[RM_USB_EP_NUM / 4] == RM_USB_EP_DIR);
        assert(regs[RM_USB_CTRL / 4] == RM_USB_FIFO_EN);
    }
    for (unsigned count = 1; count <= 64; ++count) {
        for (unsigned short_buffer = 0; short_buffer < 2; ++short_buffer) {
            unsigned capacity = count - short_buffer;
            struct rm_request req = { .req = { .buf = output + 1, .length = capacity } };
            struct rm_ep ep = { .usb = &usb, .ep = { .maxpacket = 64 }, .address = 2, .busy = true };
            memset(output, 0xa5, sizeof(output));
            memcpy(rx, pattern, count); rx_count = count; rx_cursor = 0;
            pending = &req; completions = 0;
            regs[RM_USB_STAT / 4] = RM_USB_ACK;
            regs[RM_USB_RX_COUNT / 4] = count;
            handle_out(&ep);
            assert(rx_cursor == count && req.req.actual == capacity);
            assert(completions == 1 && completion_status == (short_buffer ? -EOVERFLOW : 0));
            assert(!memcmp(output + 1, pattern, capacity));
            assert(output[0] == 0xa5 && output[capacity + 1] == 0xa5);
        }
    }
    rx_count = 8; rx_cursor = 0; memcpy(rx, pattern, 8);
    regs[RM_USB_CTRL / 4] = 0x44;
    read_setup(&usb);
    assert(rx_cursor == 8 && !memcmp(usb.last_setup.bytes, pattern, 8));
    assert(regs[RM_USB_EP_NUM / 4] == 0 && regs[RM_USB_CTRL / 4] == 0x40);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="rm443-usb-mmio-") as temporary:
            path = Path(temporary)
            (path / "mmio.c").write_text(harness + helpers + "\n" + receive +
                "\nstatic void read_setup(struct rm_usb *usb) {\n"
                "struct usb_ctrlrequest packet; unsigned i;\n" + setup + "\n}\n" + main)
            compiler = shutil.which("cc")
            if not compiler:
                self.skipTest("C compiler unavailable")
            subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(DRIVER.parent), str(path / "mmio.c"),
                            "-o", str(path / "mmio")], check=True)
            subprocess.run([str(path / "mmio")], check=True)


if __name__ == "__main__":
    unittest.main()
