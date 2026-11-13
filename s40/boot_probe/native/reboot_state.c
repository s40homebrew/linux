/* SPDX-License-Identifier: MIT */
/* Recover warm-reset RAM from instructions, not firmware-version addresses.
 * Both audited builds retain a reason byte, a reset-loop counter at +2, and
 * boot-policy data in this page. Reclaiming it can make S40 shut down after an
 * otherwise successful warm reset.
 * This parser only reads ROM; it never invokes the recovered entry points. */
#include "reboot_state.h"
#ifndef LINUX6303_REBOOT_SCAN_ONLY
#include "capture.h"
#endif

static uint16_t half(const uint8_t *p)
{
    return p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t word(const uint8_t *p)
{
    return half(p) | ((uint32_t)half(p + 2) << 16);
}

/* Only PC-relative call displacements and literal/ADR imm8s vary. */
static const uint8_t reset_code[] = {
    0x10,0xb5,0x04,0x00,0xba,0xf7,0xce,0xea,0x55,0x48,0x22,0x00,0x04,0x70,0xef,0x20,
    0x54,0xa1,0xff,0xf7,0xaa,0xfd,0x03,0x2c,0x01,0xd0,0x64,0xf1,0x6b,0xfe,0x01,0x20,
    0x9d,0xf6,0x96,0xf9,0x94,0xf0,0xe2,0xea,0x01,0x21,0x89,0x02,0x08,0x00,0x49,0x1e,
    0x00,0x28,0xfb,0xd1,0x51,0x4a,0x01,0x21,0x09,0x04,0x08,0x00,0x49,0x1e,0x00,0x28,
    0x02,0xd0,0x50,0x69,0x80,0x07,0xf8,0xd5,0x4d,0x48,0x0c,0x2c,0x01,0xd1,0x04,0x21,
    0x00,0xe0,0x02,0x21,0x41,0x61,0xfe,0xe7
};
static const uint8_t reset_calls[] = {4,18,26,32,36};
static const uint8_t reset_literals[] = {8,16,52,72};
static const uint8_t counter_code[] = {
    0x70,0xb5,0x1e,0x4c,0x00,0x25,0x28,0x00,0x65,0x70,0x84,0xf5,0xd8,0xfe,0x06,0x00,
    0x00,0x21,0x08,0x00,0x84,0xf5,0xde,0xfe,0x0a,0x2e,0x01,0xd1,0x03,0x20,0x0d,0xe0,
    0x00,0x21,0x0d,0x20,0x87,0xf5,0xa8,0xfb,0x81,0x68,0x08,0x02,0x00,0x0e,0x01,0x28,
    0x04,0xd0,0x02,0x28,0x02,0xd0,0x08,0x0e,0x04,0x28,0x01,0xd1,0x60,0x70,0xa5,0x70,
    0xff,0xf7,0xb5,0xff,0x00,0x28,0x05,0xd1,0x1a,0x48,0xa5,0x70,0x05,0x60,0x19,0x48,
    0x05,0x60,0x70,0xbd,0xa0,0x78,0x40,0x1c,0x00,0x06,0x00,0x0e,0x06,0x28,0xa0,0x70,
    0xf7,0xd3,0xef,0x21,0x15,0xa0,0xff,0xf7,0xa4,0xfc,0x00,0x20,0xff,0xf7,0x83,0xff
};
static const uint8_t counter_calls[] = {10,20,36,64,102,108};
static const uint8_t counter_literals[] = {2,72,78,100};

static int match(const uint8_t *p, const uint8_t *pattern, unsigned length,
                 const uint8_t *calls, unsigned ncalls,
                 const uint8_t *literals, unsigned nliterals)
{
    unsigned i, j;
    for (i = 0; i < length; ++i) {
        int masked = 0;
        for (j = 0; j < ncalls; ++j)
            if (i >= calls[j] && i < calls[j] + 4u)
                masked = 1;
        for (j = 0; j < nliterals; ++j)
            if (i == literals[j])
                masked = 1;
        if (!masked && p[i] != pattern[i])
            return 0;
    }
    for (j = 0; j < ncalls; ++j) {
        i = calls[j];
        if ((half(p + i) & 0xf800) != 0xf000 ||
            (half(p + i + 2) & 0xf800) != (half(pattern + i + 2) & 0xf800))
            return 0;
    }
    return 1;
}

/* The ROM base must be word aligned. All addresses below are ROM offsets. */
static uint32_t pc_relative(const uint8_t *rom, uint32_t offset)
{
    return ((offset + 4) & ~3u) + ((uint32_t)rom[offset] << 2);
}

static int literal(const uint8_t *rom, uint32_t bytes, uint32_t offset,
                   uint32_t *value)
{
    uint32_t address = pc_relative(rom, offset);
    if (address > bytes - 4)
        return 0;
    *value = word(rom + address);
    return 1;
}

static int string_at(const uint8_t *rom, uint32_t bytes, uint32_t address,
                     const char *text)
{
    unsigned i = 0;
    do {
        if (address >= bytes || rom[address++] != (uint8_t)text[i])
            return 0;
    } while (text[i++]);
    return 1;
}

uint32_t linux6303_reboot_reason_va(const uint8_t *rom, uint32_t base,
                                  uint32_t bytes)
{
    uint32_t found = 0;
    if (!rom || (base & 3) || bytes < 512 || bytes > 0xffffffffu - base)
        return 0;
    for (uint32_t offset = 0; offset <= bytes - 512; offset += 2) {
        if (rom[offset] != 0x10 || rom[offset + 1] != 0xb5 ||
            !match(rom + offset, reset_code, sizeof(reset_code),
                   reset_calls, sizeof(reset_calls), reset_literals, sizeof(reset_literals)))
            continue;
        uint32_t reason, wait_base, reset_base;
        if (!literal(rom, bytes, offset + 8, &reason) ||
            !literal(rom, bytes, offset + 52, &wait_base) ||
            !literal(rom, bytes, offset + 72, &reset_base) ||
            wait_base != 0xffd50000u || reset_base != 0xffd32d00u ||
            reason < 0xd8000000u || reason >= 0xda000000u || (reason & 3) ||
            (reason & 4095) > 4096 - LINUX6303_REBOOT_TOKEN_OFFSET - 4 ||
            !string_at(rom, bytes, pc_relative(rom, offset + 16), "hw_reset_generate(%d)"))
            continue;
        unsigned counters = 0;
        for (uint32_t c = offset + 0xb0; c <= offset + 0x140; c += 2) {
            uint32_t other, extra4, extra8;
            if (c > bytes - sizeof(counter_code) ||
                !match(rom + c, counter_code, sizeof(counter_code),
                       counter_calls, sizeof(counter_calls), counter_literals, sizeof(counter_literals)))
                continue;
            if (literal(rom, bytes, c + 2, &other) && other == reason &&
                literal(rom, bytes, c + 72, &extra4) && extra4 == reason + 4 &&
                literal(rom, bytes, c + 78, &extra8) && extra8 == reason + 8 &&
                string_at(rom, bytes, pc_relative(rom, c + 100), "HW system init reset loop"))
                ++counters;
        }
        if (counters != 1)
            continue;
        if (found)
            return 0; /* Ambiguity is not resolved by picking the first match. */
        found = reason;
    }
    return found;
}

#ifndef LINUX6303_REBOOT_SCAN_ONLY
uint32_t linux6303_reboot_reason_pa(const struct mmu_cpu *cpu)
{
    uint32_t va = linux6303_reboot_reason_va(
        (const uint8_t *)(uintptr_t)LINUX6303_REBOOT_ROM_BASE,
        LINUX6303_REBOOT_ROM_BASE, LINUX6303_REBOOT_ROM_BYTES);
    struct mmu_range page;
    if (!va)
        return 0;
    linux6303_translate_range(cpu, va & ~4095u, 4096, &page);
    if (page.status || (page.pa & 4095) || page.pa < 0x10000000u ||
        page.pa > 0x11fff000u)
        return 0;
    if (*(const volatile uint32_t *)(uintptr_t)(va + LINUX6303_REBOOT_TOKEN_OFFSET) !=
        LINUX6303_REBOOT_TOKEN)
        return 0;
    return page.pa + (va & 4095);
}
#endif
