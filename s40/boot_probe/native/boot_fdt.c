/* SPDX-License-Identifier: MIT */
/* Device tree for the Nokia 6303 classic. */
#include <stdio.h>
#include <string.h>
#include "boot_fdt.h"
#include "../../kernel/overlay/arch/arm/mach-nokia-raps/rm443_secure_wdt_abi.h"

static const char names[] = "compatible\0model\0#address-cells\0#size-cells\0bootargs\0"
    "device_type\0reg\0ranges\0no-map\0interrupt-controller\0#interrupt-cells\0"
    "phandle\0interrupt-parent\0interrupts\0clock-frequency\0reg-names\0"
    "interrupt-names\0dr_mode\0maximum-speed\0bus-width\0max-frequency\0"
    "nokia,reset-reason-offset\0#dma-cells\0dmas\0dma-names\0";
struct writer { uint8_t *p; size_t at, cap; int failed; };
static void word(struct writer *w, uint32_t v) {
    if (w->at + 4 > w->cap) { w->failed = 1; return; }
    w->p[w->at++] = v >> 24; w->p[w->at++] = v >> 16;
    w->p[w->at++] = v >> 8; w->p[w->at++] = v;
}
static void bytes(struct writer *w, const void *p, size_t length) {
    size_t padded = (length + 3) & ~(size_t)3;
    if (padded > w->cap - w->at) { w->failed = 1; return; }
    if (length) memcpy(w->p + w->at, p, length);
    memset(w->p + w->at + length, 0, padded - length);
    w->at += padded;
}
static void node(struct writer *w, const char *name) { word(w, 1); bytes(w, name, strlen(name) + 1); }
static void end(struct writer *w) { word(w, 2); }
static void property(struct writer *w, const char *name, size_t length) {
    size_t offset = 0;
    while (offset < sizeof(names) && strcmp(names + offset, name))
        offset += strlen(names + offset) + 1;
    if (offset >= sizeof(names)) { w->failed = 1; return; }
    word(w, 3); word(w, length); word(w, offset);
}
static void string(struct writer *w, const char *name, const char *s) {
    property(w, name, strlen(s) + 1); bytes(w, s, strlen(s) + 1);
}
static void cell(struct writer *w, const char *name, uint32_t v) {
    property(w, name, 4); word(w, v);
}
static void region(struct writer *w, uint32_t base, uint32_t size) {
    property(w, "reg", 8); word(w, base); word(w, size);
}

static int overlap(uint32_t a, uint32_t n, uint32_t b, uint32_t m) {
    return a < b + m && b < a + n;
}

static size_t fdt_with_state(void *buffer, size_t capacity,
    uint32_t base, uint32_t size, uint32_t transition, uint32_t reason,
    uint32_t secure, uint32_t work, uint32_t work_bytes,
    const struct rm443_swd_state *state, unsigned options) {
    /* ARM926 linear mapping needs section-compatible RAM. A retained block
     * may live anywhere in DDR: full takeover must not discard low RAM just
     * because the firmware allocator placed transition code near the top. */
    if (options & ~(LINUX6303_OPTION_DMA | LINUX6303_OPTION_COLOR_MASK) ||
        (options & LINUX6303_OPTION_COLOR_MASK) == LINUX6303_OPTION_COLOR_MASK)
        return 0;
    if (!buffer || capacity < 4096 || (base & 0x1fffff) ||
        size < 0x200000 || (size & 0x1fffff) || base < 0x10000000 ||
        base > 0x12000000 || size > 0x12000000 - base ||
        (transition & 0x3fff) || transition < base ||
        transition > base + size - 0x10000)
        return 0;
    uint32_t reboot_page = reason & ~0xfffu;
    if (reason && (reboot_page < base || reboot_page > base + size - 0x1000 ||
        (reason & 3) || (reason & 0xfff) > 4096 - 0x128 ||
        (reboot_page < transition + 0x10000 && transition < reboot_page + 0x1000)))
        return 0;
    if (secure && ((secure & 0x3fff) || secure < base ||
        secure > base + size - RM443_SWD_BYTES || (work & 4095) ||
        !work_bytes || (work_bytes & 4095) || work_bytes > size ||
        work < base || work > base + size - work_bytes ||
        overlap(secure, RM443_SWD_BYTES, transition, 0x10000) ||
        overlap(work, work_bytes, transition, 0x10000) ||
        overlap(secure, RM443_SWD_BYTES, work, work_bytes) ||
        (reason && (overlap(secure, RM443_SWD_BYTES, reboot_page, 4096) ||
                    overlap(work, work_bytes, reboot_page, 4096)))))
        return 0;
    if (!secure && (work || work_bytes))
        return 0;
    if (state) {
        if (!secure) return 0;
        for (unsigned i = 0; i < RM443_SWD_POOL_SLOTS; ++i) {
            uint32_t pa = state->firmware_alloc_pa[i];
            uint32_t n = state->firmware_alloc_bytes[i];
            if (!pa && !n) continue;
            if (!n || n > size || pa < base || pa > base + size - n ||
                overlap(pa, n, transition, 0x10000) ||
                overlap(pa, n, secure, RM443_SWD_BYTES)) return 0;
        }
    }
    struct writer w = {buffer, 56, capacity, 0};
    memset(buffer, 0, 56); /* header + terminating 64-bit reserve-map pair */
    char text[640];
    node(&w, "");
    string(&w, "compatible", "nokia,rm443");
    string(&w, "model", "Nokia 6303 classic");
    cell(&w, "#address-cells", 1); cell(&w, "#size-cells", 1);
    node(&w, "chosen");
    snprintf(text, sizeof(text), "earlycon=rm443ram,mmio32,0x%08x console=tty0 consoleblank=0 fbcon=font:VGA8x16 vt.global_cursor_default=0 console=ttyRM0 console=ttyGS0 loglevel=7 panic=0 printk.devkmsg=on g_cdc.dev_addr=02:00:00:44:43:02 g_cdc.host_addr=02:00:00:44:43:01 g_cdc.qmult=1 g_cdc.iManufacturer=Linux g_cdc.iProduct=\"Nokia 6303\" coherent_pool=16K rm443.display=%s rm443.lcd=%s",
        transition + 0xf000,
        LINUX6303_DISPLAY_DMA && (options & LINUX6303_OPTION_DMA) ? "dma" : "pio",
        (options & LINUX6303_OPTION_COLOR_MASK) == LINUX6303_OPTION_RGB888 ? "rgb888" :
        (options & LINUX6303_OPTION_COLOR_MASK) == LINUX6303_OPTION_RGB666 ? "rgb666" : "rgb565");
    if (LINUX6303_INSPECT)
        strncat(text, " rm443.inspect=1", sizeof(text) - strlen(text) - 1);
    if (!LINUX6303_SDLOG)
        strncat(text, " rm443.sdlog=0", sizeof(text) - strlen(text) - 1);
    if (LINUX6303_USB_WAIT_MS) {
        size_t length = strlen(text);
        snprintf(text + length, sizeof(text) - length,
                 " rm443_usb.boot_wait_ms=%u", (unsigned)LINUX6303_USB_WAIT_MS);
    }
    if (LINUX6303_SD_POLL)
        strncat(text, " rm443_mmc.poll=1", sizeof(text) - strlen(text) - 1);
    if (LINUX6303_SD_TRACE)
        strncat(text, " rm443_mmc.trace=1", sizeof(text) - strlen(text) - 1);
    if (LINUX6303_NOHLT)
        strncat(text, " nohlt", sizeof(text) - strlen(text) - 1);
    string(&w, "bootargs", text); end(&w);
    node(&w, "cpus"); cell(&w, "#address-cells", 1); cell(&w, "#size-cells", 0);
    node(&w, "cpu@0"); string(&w, "device_type", "cpu"); string(&w, "compatible", "arm,arm926ej-s");
    cell(&w, "reg", 0); end(&w); end(&w);
    snprintf(text, sizeof(text), "memory@%x", base);
    node(&w, text); string(&w, "device_type", "memory"); region(&w, base, size); end(&w);
    node(&w, "interrupt-controller@40000000");
    string(&w, "compatible", "nokia,rm443-intc"); region(&w, 0x40000000, 0x1000);
    property(&w, "interrupt-controller", 0); cell(&w, "#interrupt-cells", 1);
    cell(&w, "#address-cells", 0); cell(&w, "phandle", 1); end(&w);
    node(&w, "timer@20040000");
    string(&w, "compatible", "nokia,rm443-timer"); region(&w, 0x20040000, 0x498);
    cell(&w, "interrupt-parent", 1); cell(&w, "interrupts", 36);
    cell(&w, "clock-frequency", 9600000); end(&w);
    node(&w, "display@20022200");
    string(&w, "compatible", "nokia,rm443-lcd"); region(&w, 0x20022200, 0x20);
    if (LINUX6303_DISPLAY_DMA) {
        property(&w, "dmas", 8); word(&w, 2); word(&w, 9);
        string(&w, "dma-names", "tx");
    }
    end(&w);
    if (LINUX6303_DISPLAY_DMA) {
        node(&w, "dma-controller@20040c00");
        string(&w, "compatible", "nokia,raps-dma");
        property(&w, "reg", 24);
        word(&w, 0x20040c00); word(&w, 0x1800);
        word(&w, 0x20040804); word(&w, 0x1c);
        word(&w, 0x20040780); word(&w, 0xc);
        cell(&w, "#dma-cells", 1); cell(&w, "phandle", 2);
        cell(&w, "interrupt-parent", 1);
        property(&w, "interrupts", 24);
        for (unsigned irq = 0; irq < 6; ++irq) word(&w, irq);
        end(&w);
    }
    node(&w, "keypad@20031cc0");
    string(&w, "compatible", "nokia,rm443-keypad"); region(&w, 0x20031cc0, 0xe4); end(&w);
    node(&w, "power@20030700");
    string(&w, "compatible", "nokia,rm443-power");
    property(&w, "reg", 40);
    word(&w, 0x20030700); word(&w, 0x40);
    word(&w, 0x20040204); word(&w, 4);
    word(&w, 0x20040500); word(&w, 0xc);
    word(&w, 0x20032c18); word(&w, 4);
    word(&w, 0x20032d14); word(&w, 4);
    static const char power_regs[] = "cbus\0semaphore\0watchdog\0reset-status\0reset-control";
    property(&w, "reg-names", sizeof(power_regs)); bytes(&w, power_regs, sizeof(power_regs));
    end(&w);
    node(&w, "usb@20036600");
    string(&w, "compatible", "nokia,rm443-usb");
    property(&w, "reg", 40);
    word(&w, 0x20036600); word(&w, 0x100);
    word(&w, 0x20036700); word(&w, 0x100);
    word(&w, 0x20032c80); word(&w, 0x104);
    word(&w, 0x2004020c); word(&w, 4);
    word(&w, 0x20031c68); word(&w, 4);
    static const char usb_regs[] = "udc\0otg\0clocks\0semaphore\0pinmux";
    property(&w, "reg-names", sizeof(usb_regs)); bytes(&w, usb_regs, sizeof(usb_regs));
    cell(&w, "interrupt-parent", 1);
    property(&w, "interrupts", 8); word(&w, 153); word(&w, 154);
    static const char usb_irqs[] = "general\0endpoint";
    property(&w, "interrupt-names", sizeof(usb_irqs)); bytes(&w, usb_irqs, sizeof(usb_irqs));
    string(&w, "dr_mode", "peripheral");
    string(&w, "maximum-speed", "full-speed"); end(&w);
    node(&w, "mmc@20032700");
    string(&w, "compatible", "nokia,rm443-mmc"); region(&w, 0x20032700, 0x80);
    cell(&w, "interrupt-parent", 1); cell(&w, "interrupts", 139);
    cell(&w, "bus-width", LINUX6303_SD_SAFE ? 1 : 4);
    cell(&w, "clock-frequency", 96000000);
    cell(&w, "max-frequency", LINUX6303_SD_SAFE ? 400000 : 12500000); end(&w);
    node(&w, "reserved-memory"); cell(&w, "#address-cells", 1); cell(&w, "#size-cells", 1);
    property(&w, "ranges", 0);
    snprintf(text, sizeof(text), "transition@%x", transition);
    node(&w, text); region(&w, transition, 0xf000); end(&w);
    snprintf(text, sizeof(text), "ramlog@%x", transition + 0xf000);
    node(&w, text); region(&w, transition + 0xf000, 0x1000); end(&w);
    if (reason) {
        snprintf(text, sizeof(text), "reboot-state@%x", reboot_page);
        node(&w, text);
        string(&w, "compatible", "nokia,rm443-reboot-state");
        region(&w, reboot_page, 0x1000);
        cell(&w, "nokia,reset-reason-offset", reason & 0xfff);
        end(&w);
    }
    if (secure) {
        snprintf(text, sizeof(text), "secure-watchdog@%x", secure);
        node(&w, text);
        string(&w, "compatible", "nokia,rm443-secure-watchdog");
        region(&w, secure, RM443_SWD_BYTES); end(&w);
        snprintf(text, sizeof(text), "secure-work@%x", work);
        node(&w, text);
        string(&w, "compatible", "nokia,rm443-secure-work");
        region(&w, work, work_bytes); end(&w);
        if (state) {
            for (unsigned i = 0; i < RM443_SWD_POOL_SLOTS; ++i) {
                uint32_t pa = state->firmware_alloc_pa[i];
                uint32_t n = state->firmware_alloc_bytes[i];
                if (!pa) continue;
                uint32_t page = pa & ~4095u;
                uint32_t length = ((pa & 4095) + n + 4095) & ~4095u;
                /* Different firmware allocations can share one page. The
                 * reservations may overlap; payload ownership does not. */
                snprintf(text, sizeof(text), "secure-allocation%u@%x", i, page);
                node(&w, text);
                string(&w, "compatible", "nokia,rm443-secure-allocation");
                region(&w, page, length); end(&w);
            }
        }
    }
    end(&w); end(&w); word(&w, 9);
    size_t structure_bytes = w.at - 56, strings_at = w.at;
    bytes(&w, names, sizeof(names));
    if (w.failed) return 0;
    size_t total = w.at;
    w.at = 0;
    word(&w, 0xd00dfeed); word(&w, total); word(&w, 56); word(&w, strings_at);
    word(&w, 40); word(&w, 17); word(&w, 16); word(&w, 0);
    word(&w, sizeof(names)); word(&w, structure_bytes);
    return total;
}

size_t linux6303_fdt_with_secure_watchdog(void *buffer, size_t capacity,
    uint32_t base, uint32_t size, uint32_t transition, uint32_t reason,
    uint32_t secure, uint32_t work, uint32_t work_bytes) {
    return fdt_with_state(buffer, capacity, base, size, transition, reason,
                          secure, work, work_bytes, NULL, 0);
}

size_t linux6303_fdt_with_secure_state(void *buffer, size_t capacity,
    uint32_t base, uint32_t size, uint32_t transition, uint32_t reason,
    uint32_t secure, const struct rm443_swd_state *state) {
    return linux6303_fdt_with_options(buffer, capacity, base, size, transition,
                                      reason, secure, state, 0);
}

size_t linux6303_fdt_with_options(void *buffer, size_t capacity,
    uint32_t base, uint32_t size, uint32_t transition, uint32_t reason,
    uint32_t secure, const struct rm443_swd_state *state, unsigned options) {
    if (!state || !state->work_bytes || state->work_bytes > 0x10000)
        return 0;
    uint32_t page = state->work_pa & ~4095u;
    uint32_t length = ((state->work_pa & 4095) + state->work_bytes + 4095) & ~4095u;
    return fdt_with_state(buffer, capacity, base, size, transition, reason,
                          secure, page, length, state, options);
}

size_t linux6303_fdt_with_reboot(void *buffer, size_t capacity, uint32_t base,
                               uint32_t size, uint32_t transition, uint32_t reason) {
    return linux6303_fdt_with_secure_watchdog(buffer, capacity, base, size,
        transition, reason, 0, 0, 0);
}

size_t linux6303_fdt_at(void *buffer, size_t capacity, uint32_t base, uint32_t size,
                       uint32_t transition) {
    return linux6303_fdt_with_reboot(buffer, capacity, base, size, transition, 0);
}

size_t linux6303_fdt(void *buffer, size_t capacity, uint32_t base, uint32_t size) {
    if (size < 0x10000 || base > UINT32_MAX - size) return 0;
    return linux6303_fdt_at(buffer, capacity, base, size, base + size - 0x10000);
}
