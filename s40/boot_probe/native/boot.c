/* SPDX-License-Identifier: GPL-2.0-or-later */
/* One-way Linux boot. Physical userspace works, but kernel text corruption
 * was observed in the DSP shared DDR window. Exclude low 2 MiB until runtime
 * DSP shutdown is established; checksum Image at each ownership boundary. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s40/runtime.h"
#include "firmware_api.h"
#include "capture.h"
#include "boot_fdt.h"
#include "reboot_state.h"
#include "secure_watchdog.h"
#include "payload_layout.h"
#include "../../handoff/relocation_abi.h"
#include "../../handoff/image_hash.h"
#include "firmware.h"
#include "../../kernel/overlay/arch/arm/mach-nokia-raps/rm443_boot_marker.h"

extern const uint8_t linux_image_start[], linux_image_end[], handoff_start[], handoff_end[];

/* S40 still owns the machine here. Use its normal GDI ownership path rather
 * than racing it with direct LCD register writes. Only a 64x40 patch is needed;
 * allocating the shared full-screen display would add another 300 KiB arena.
 * After takeover, stackless physical code draws the same two-digit glyphs. */
/* The firmware bitmap reader also uses the four bytes BEFORE pixels. */
static struct {
    uint16_t format, width;
    uint32_t pixels[64 * 40];
} marker_bitmap;
static int marker_token = -1;
static struct {
    uint32_t *pixels;
    uint16_t width, height;
    uint8_t unused[2], format, pad;
    uint32_t extra[3];
} marker_canvas;

static void marker_close(void) {
    if (marker_token != -1) {
        fw_gdi_display_master_reserve_disable(1, marker_token);
        marker_token = -1;
    }
}

static void checkpoint(unsigned code) {
    if (marker_token == -1) {
        if (*fw_lcd_owner != -1)
            return; /* diagnostic display unavailable; preserve boot behavior */
        marker_token = fw_gdi_display_master_reserve_enable(1);
        if (marker_token == -1)
            return;
        marker_bitmap.format = 9;
        marker_bitmap.width = 64;
        marker_canvas.pixels = marker_bitmap.pixels;
        marker_canvas.width = 64;
        marker_canvas.height = 40;
        marker_canvas.format = 12;
        s40_on_exit(marker_close, 1);
    }
    for (unsigned y = 0; y < 40; ++y)
        for (unsigned x = 0; x < 64; ++x)
            marker_bitmap.pixels[y * 64 + x] = rm443_marker_pixel(code, x, y)
                    ? 0xffffffffu : 0xff000000u;
    fw_gdi_display_master_update(&marker_canvas, 1, 0, 0, 64, 40, 0);
    /* This pause is before takeover, with firmware scheduling/watchdogs alive.
     * No such delay is introduced after interrupts/MMU have changed. */
    s40_sleep_ms(250);
}

static void transition_table(uint32_t *table, uint32_t table_pa, uint32_t va, uint32_t pa) {
    memset(table, 0, 19456);
    uint32_t coarse0 = table_pa + 16384, coarse1 = coarse0 + 1024;
    table[va >> 20] = coarse0 | 0x11;
    table[4096 + ((va >> 12) & 255)] = pa | 0x552;
    if ((pa >> 20) == (va >> 20))
        table[4096 + ((pa >> 12) & 255)] = pa | 0x552;
    else {
        table[pa >> 20] = coarse1 | 0x11;
        table[4352 + ((pa >> 12) & 255)] = pa | 0x552;
    }
    /* Stage39 runs after TTBR replacement but before MMU disable. Map only
     * the LCD and GPIO pages it needs, using a third coarse table. */
    table[0x200] = (table_pa + 18432) | 0x11;
    table[4608 + 0x22] = 0x20022552;
    table[4608 + 0x31] = 0x20031552;
}

/* Exercise the actual copied ARM marker while firmware translation is still
 * available. Restore the LCD pins and the firmware's RGB888 panel format
 * before yielding to S40. No firmware callbacks run with IRQ/FIQ masked.
 * Completion proves bus transfers, not that the panel displayed the glyph. */
static int marker_preflight(uint8_t *stub, uint32_t lcd_va) {
    volatile uint32_t *lcd = (void *)(uintptr_t)lcd_va;
    volatile uint32_t *gpio = (void *)(uintptr_t)(lcd_va + 0xfa00);
    uint32_t cpsr, mask;
    __asm__ volatile("mrs %0, cpsr\norr %1, %0, #0xc0\nmsr cpsr_c, %1"
        : "=&r"(cpsr), "=&r"(mask) : : "memory", "cc");
    uint32_t mux = gpio[0x20 / 4] & 0xffff;
    uint32_t direction = gpio[0xe0 / 4] & 0xff0000;
    uint32_t idle = lcd[8 / 4] & 1;
    int (*draw)(unsigned, uint32_t) = (void *)(stub + HANDOFF_MARKER_CALL_OFFSET);
    int status = draw(0x35, lcd_va);
    unsigned mode = lcd[4 / 4] & 0x3ff00000;
    /* S40's F0251E46/F0253BAC paths send three bytes per framebuffer pixel. */
    int restored = rm443_marker_transfer((void *)lcd, mode, 0, 1, 0x3a) ||
        rm443_marker_transfer((void *)lcd, mode, 0x40000, 1, 0x66) ||
        rm443_marker_ready((void *)lcd);
    gpio[0x28 / 4] = 0xffff;
    gpio[0x24 / 4] = mux;
    gpio[0xe8 / 4] = 0xff0000;
    gpio[0xe4 / 4] = direction;
    lcd[8 / 4] = (lcd[8 / 4] & ~1u) | idle;
    __asm__ volatile("msr cpsr_c, %0" : : "r"(cpsr) : "memory", "cc");
    return status ? status : restored ? -2 : 0;
}

static int overlaps(uint32_t a, uint32_t bytes, uint32_t b, uint32_t other) {
    return a < b + other && b < a + bytes;
}

int s40_app_main(void) {
    enum { DDR_BASE = 0x10000000, DDR_BYTES = 0x2000000,
           LINUX_BASE = DDR_BASE + 0x200000, LINUX_BYTES = DDR_BYTES - 0x200000,
           SCRATCH_BYTES = 0x10000, ALLOC_BYTES = 0x14000,
           IMAGE_PA = LINUX_BASE + 0x8000 };
    struct mmu_cpu cpu = {0};
    struct mmu_range physical, image;
    checkpoint(0x30);
    if (!linux6303_find_table_walker()) s40_error("MMU policy signature missing/ambiguous");
    int status = linux6303_mmu_cpu(&cpu);
    if (status || cpu.fcse) s40_error("CPU state rejected %d FCSE=%08x", status, cpu.fcse);
    checkpoint(0x31);
    size_t image_bytes = (uintptr_t)linux_image_end - (uintptr_t)linux_image_start;
    size_t stub_bytes = (uintptr_t)handoff_end - (uintptr_t)handoff_start;
    if (image_bytes != LINUX_IMAGE_BYTES || LINUX_MEMORY_BYTES < image_bytes ||
        LINUX_MEMORY_BYTES > LINUX_BYTES - 0x8000 - SCRATCH_BYTES ||
        stub_bytes != HANDOFF_BYTES || stub_bytes > 4096)
        s40_error("Payload layout invalid");
    /* Translate the native application's live embedded Image rather than
     * allocating and copying another giant staging arena. */
    linux6303_translate_range(&cpu, (uint32_t)(uintptr_t)linux_image_start,
                             image_bytes, &image);
    if (image.status) s40_error("Image not contiguous: %u at %08x", image.status, image.first_bad_va);
    uint32_t native_hash = linux6303_image_hash(linux_image_start, image_bytes);
    if (native_hash != LINUX_IMAGE_HASH) {
        checkpoint(0x3c);
        s40_error("Image damaged before takeover: %08x expected %08x", native_hash, LINUX_IMAGE_HASH);
    }
    checkpoint(0x32);
    void *allocation = malloc(ALLOC_BYTES);
    if (!allocation) s40_error("Cannot allocate 80 KiB transition scratch");
    linux6303_translate_range(&cpu, (uint32_t)(uintptr_t)allocation, ALLOC_BYTES, &physical);
    if (physical.status) s40_error("Scratch not contiguous: %u at %08x", physical.status, physical.first_bad_va);
    uint32_t lcd_va = linux6303_mmio_alias(&cpu, 0x20022200);
    uint32_t gpio_va = linux6303_mmio_alias(&cpu, 0x20031c00);
    if (!lcd_va || gpio_va != lcd_va + 0xfa00) {
        checkpoint(0x3d);
        s40_error("LCD/GPIO uncached aliases unavailable");
    }
    uint32_t scratch_pa = (physical.pa + 0x3fff) & ~0x3fffu;
    uint32_t offset = scratch_pa - physical.pa;
    if (offset > ALLOC_BYTES - SCRATCH_BYTES || scratch_pa < LINUX_BASE ||
        scratch_pa > DDR_BASE + DDR_BYTES - SCRATCH_BYTES ||
        overlaps(scratch_pa, SCRATCH_BYTES, LINUX_BASE, 0x8000 + LINUX_MEMORY_BYTES) ||
        overlaps(scratch_pa, SCRATCH_BYTES, image.pa, image_bytes))
        s40_error("Transition scratch overlaps payload or destination");
    uint32_t reboot_reason = linux6303_reboot_reason_pa(&cpu);
    uint32_t reboot_page = reboot_reason & ~0xfffu;
    /* The early ARM page directory occupies base+0x4000..base+0x7fff.
     * 08.90's reset state is below it at base+0x1000; retain that page too. */
    if (reboot_reason && (reboot_page < LINUX_BASE ||
        overlaps(reboot_page, 4096, LINUX_BASE + 0x4000, 0x4000 + LINUX_MEMORY_BYTES) ||
        overlaps(reboot_page, 4096, scratch_pa, SCRATCH_BYTES) ||
        overlaps(reboot_page, 4096, image.pa, image_bytes)))
        s40_error("Firmware reboot state overlaps Linux payload");
    struct linux6303_swd_symbols secure_symbols;
    if (!linux6303_swd_discover((const uint8_t *)LINUX6303_SWD_ROM_BASE,
            LINUX6303_SWD_ROM_BASE, LINUX6303_SWD_ROM_BYTES, &secure_symbols))
        s40_error("Secure watchdog signature missing/ambiguous");
    void *secure_allocation = malloc(RM443_SWD_BYTES + 0x4000);
    if (!secure_allocation) s40_error("Cannot allocate secure watchdog memory");
    struct mmu_range secure_range;
    linux6303_translate_range(&cpu, (uint32_t)(uintptr_t)secure_allocation,
                             RM443_SWD_BYTES + 0x4000, &secure_range);
    if (secure_range.status) s40_error("Secure watchdog memory is not contiguous");
    uint32_t secure_pa = (secure_range.pa + 0x3fff) & ~0x3fffu;
    uint8_t *secure_memory = (uint8_t *)secure_allocation + secure_pa - secure_range.pa;
    if (secure_pa < LINUX_BASE || secure_pa > DDR_BASE + DDR_BYTES - RM443_SWD_BYTES ||
        overlaps(secure_pa, RM443_SWD_BYTES, LINUX_BASE, 0x8000 + LINUX_MEMORY_BYTES) ||
        overlaps(secure_pa, RM443_SWD_BYTES, image.pa, image_bytes) ||
        overlaps(secure_pa, RM443_SWD_BYTES, scratch_pa, SCRATCH_BYTES) ||
        (reboot_reason && overlaps(secure_pa, RM443_SWD_BYTES, reboot_page, 4096)))
        s40_error("Secure watchdog memory overlaps boot data");
    memset(secure_memory, 0, RM443_SWD_BYTES);
    struct rm443_swd_state *secure = (void *)secure_memory;
    status = linux6303_swd_capture(&cpu, &secure_symbols, secure);
    if (status) s40_error("Secure watchdog state unavailable: %d", status);
    secure->block_pa = secure_pa;
    uint32_t work_page = secure->work_pa & ~4095u;
    uint32_t work_bytes = ((secure->work_pa & 4095) + secure->work_bytes + 4095) & ~4095u;
    if (work_page < LINUX_BASE || work_bytes > LINUX_BYTES ||
        work_page > DDR_BASE + DDR_BYTES - work_bytes ||
        overlaps(work_page, work_bytes, LINUX_BASE + 0x4000, 0x4000 + LINUX_MEMORY_BYTES) ||
        overlaps(work_page, work_bytes, image.pa, image_bytes) ||
        overlaps(work_page, work_bytes, scratch_pa, SCRATCH_BYTES) ||
        overlaps(work_page, work_bytes, secure_pa, RM443_SWD_BYTES) ||
        (reboot_reason && overlaps(work_page, work_bytes, reboot_page, 4096)))
        s40_error("Secure service state overlaps boot data");
    for (unsigned i = 0; i < RM443_SWD_POOL_SLOTS; ++i) {
        uint32_t pa = secure->firmware_alloc_pa[i];
        uint32_t bytes = secure->firmware_alloc_bytes[i];
        if (!pa && !bytes) continue;
        if (!bytes || bytes > LINUX_BYTES || pa < LINUX_BASE ||
            pa > DDR_BASE + DDR_BYTES - bytes ||
            overlaps(pa, bytes, LINUX_BASE + 0x4000, 0x4000 + LINUX_MEMORY_BYTES) ||
            overlaps(pa, bytes, image.pa, image_bytes) ||
            overlaps(pa, bytes, scratch_pa, SCRATCH_BYTES) ||
            overlaps(pa, bytes, secure_pa, RM443_SWD_BYTES) ||
            overlaps(pa, bytes, secure->work_pa, secure->work_bytes))
            s40_error("Secure allocation %u overlaps boot data: %08x+%x", i, pa, bytes);
        for (unsigned j = 0; j < i; ++j)
            if (secure->firmware_alloc_pa[j] && overlaps(pa, bytes,
                    secure->firmware_alloc_pa[j], secure->firmware_alloc_bytes[j]))
                s40_error("Secure allocations overlap");
    }
    if (!linux6303_swd_map(&cpu, 0xfff00000,
            (uint32_t *)(secure_memory + RM443_SWD_ROM_MAP_OFFSET)) ||
        !linux6303_swd_map(&cpu, 0xffd00000,
            (uint32_t *)(secure_memory + RM443_SWD_IO_MAP_OFFSET)))
        s40_error("Secure service mappings unavailable");
    linux6303_swd_map(&cpu, 0xffc00000,
        (uint32_t *)(secure_memory + RM443_SWD_FFC_MAP_OFFSET));
    linux6303_swd_map(&cpu, 0xffe00000,
        (uint32_t *)(secure_memory + RM443_SWD_FFE_MAP_OFFSET));
    uint8_t *scratch = (uint8_t *)allocation + offset;
    if ((uint32_t)(uintptr_t)scratch & 0xfff)
        s40_error("Scratch VA/PA page offsets disagree");
    memset(scratch, 0, SCRATCH_BYTES);
    struct linux6303_boot_trace *trace = (void *)(scratch + 0xe000);
    *trace = (struct linux6303_boot_trace){
        .magic0 = 0x4c303453, .magic1 = 0x3130584e, /* S40LNX01 */
        .format = 3, .stage = 2,
        .base_pa = LINUX_BASE, .bytes = LINUX_BYTES,
        .log_pa = scratch_pa + 0xf000, .image_pa = IMAGE_PA,
        .stub_va = (uint32_t)(uintptr_t)(scratch + 0xd000),
        .stub_pa = scratch_pa + 0xd000, .ttbr_pa = scratch_pa + 0x8000,
        .dtb_pa = scratch_pa, .old_ttbr = cpu.ttbr, .old_domains = cpu.domains,
        .source_pa = image.pa, .scratch_pa = scratch_pa, .scratch_bytes = SCRATCH_BYTES,
        .quiesce_status = 0xffffffffu,
        .expected_hash = LINUX_IMAGE_HASH, .native_hash = native_hash,
    };
    struct linux6303_relocation *plan = (void *)(scratch + 0xe100);
    *plan = (struct linux6303_relocation){
        .source = image.pa, .destination = IMAGE_PA, .image_bytes = image_bytes,
        .memory_bytes = LINUX_MEMORY_BYTES, .dtb = trace->dtb_pa,
        .trace = scratch_pa + 0xe000, .quiesce_status = 0xffffffffu,
        .lcd_va = lcd_va,
        .expected_hash = LINUX_IMAGE_HASH,
    };
    if (!linux6303_fdt_with_options(scratch, 4096, LINUX_BASE, LINUX_BYTES,
            scratch_pa, reboot_reason, secure_pa, secure, s40_launch_options()))
        s40_error("DT construction failed");
    checkpoint(0x33);
    memcpy(scratch + 0xd000, handoff_start, stub_bytes);
    transition_table((uint32_t *)(scratch + 0x8000), trace->ttbr_pa,
                     trace->stub_va, trace->stub_pa);
    FILE *file = fopen("D:\\linux6303-boot.txt", "wb");
    if (file) {
        fprintf(file, "Warm reboot reason PA %08x; page retained if nonzero\n", reboot_reason);
        fprintf(file, "Secure watchdog: bridge %08x work %08x+%x PA %08x descriptor %08x token %u\n",
                secure_pa, secure->work_pa, secure->work_bytes,
                secure->pa_header_pa, secure->descriptor_pa, secure->token);
        for (unsigned i = 0; i < RM443_SWD_POOL_SLOTS; ++i)
            if (secure->firmware_alloc_pa[i])
                fprintf(file, "Secure allocation %u: %08x+%x\n", i,
                        secure->firmware_alloc_pa[i], secure->firmware_alloc_bytes[i]);
        /* Ordinary readback registers only; semaphore reads acquire tokens
         * and must never be included in a diagnostic register dump. */
        uint32_t mmc_va = linux6303_mmio_alias(&cpu, 0x20032700);
        uint32_t clock_va = linux6303_mmio_alias(&cpu, 0x20032c00);
        if (mmc_va && clock_va) {
            const volatile uint32_t *mmc = (void *)(uintptr_t)mmc_va;
            const volatile uint32_t *clocks = (void *)(uintptr_t)clock_va;
            const volatile uint32_t *gpio = (void *)(uintptr_t)gpio_va;
            fprintf(file, "S40 SD: CON=%08x SYSS=%08x clock=%08x gates=%08x control=%08x PLL0=%08x PLL1=%08x PLL2=%08x mux=%08x\n",
                    mmc[0x0c / 4], mmc[0x68 / 4], clocks[0x10 / 4],
                    clocks[0xd0 / 4], clocks[0x80 / 4], clocks[0x120 / 4],
                    clocks[0x140 / 4], clocks[0x180 / 4], gpio[0x10 / 4]);
        }
        fprintf(file, "ONE-WAY LINUX: RAM %08x + %08x; low 2 MiB excluded\nImage %08x -> %08x\nDTB %08x\nRAM log %08x\nRetained %08x + %08x\n",
                LINUX_BASE, LINUX_BYTES, image.pa, IMAGE_PA, trace->dtb_pa,
                trace->log_pa, scratch_pa, SCRATCH_BYTES);
        fclose(file);
    }
    void (*sync)(void *, unsigned) = (void (*)(void *, unsigned))(uintptr_t)
        s40_result[FW_API_DESCRIPTOR + API_CACHE_SYNC];
    sync(scratch + 0xd000, stub_bytes);
    checkpoint(0x34);
    status = marker_preflight(scratch + 0xd000, lcd_va);
    if (status) {
        checkpoint(0x3e);
        s40_error("Direct LCD preflight failed: %d", status);
    }
    s40_sleep_ms(400); /* let a recording distinguish direct35 from GDI36 */
    checkpoint(0x36);
    /* No low DDR byte is changed here. Private stackless code switches away
     * from firmware tables, quiesces hardware, memmoves Image physically,
     * zeroes BSS and only then records stage 3 and enters Linux. */
    int (*enter)(uint32_t, uint32_t, uint32_t, uint32_t) =
        (void *)(scratch + 0xd000 + HANDOFF_RELOCATE_OFFSET);
    /* The S40 timer may have renewed the secure token during checkpoints.
     * Read it after the last yield; keep IRQ/FIQ masked until takeover so
     * there cannot be another firmware renewal between capture and Linux.
     */
    uint32_t before, masked;
    __asm__ volatile("mrs %0, cpsr\norr %1, %0, #0xc0\nmsr cpsr_c, %1"
        : "=&r"(before), "=&r"(masked) : : "memory", "cc");
    status = linux6303_swd_refresh_token(&cpu, &secure_symbols, secure);
    if (status) {
        __asm__ volatile("msr cpsr_c, %0" : : "r"(before) : "memory", "cc");
        s40_error("Secure watchdog token capture failed: %d", status);
    }
    int result = enter((uint32_t)(uintptr_t)plan, scratch_pa + 0xe100,
                       trace->ttbr_pa, trace->stub_pa + HANDOFF_RELOCATE_IDENTITY_OFFSET);
    __asm__ volatile("msr cpsr_c, %0" : : "r"(before) : "memory", "cc");
    trace->stage = 4;
    s40_error("Handoff refused CPU state: %d", result);
}
