/* SPDX-License-Identifier: MIT */
/* Discover the RM-443 secure watchdog contract without firmware version checks.
 * Only relative call/literal encodings and an unrelated callback log code vary
 * between the audited 08.90 and 10.12 implementations. No firmware code runs. */
#include "secure_watchdog.h"
#ifndef LINUX6303_SWD_SCAN_ONLY
#include "capture.h"
#endif

static uint16_t half(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t word(const uint8_t *p) { return half(p) | ((uint32_t)half(p + 2) << 16); }

static const uint8_t wrapper_code[] = {
    0xfe,0xb5,0x05,0x00,0xa9,0x48,0xa9,0x4f,0x0e,0x00,0x00,0x2d,0x78,0x60,0x01,0xd0,
    0x00,0x2e,0x02,0xd1,0x04,0x20,0xc0,0x43,0xfe,0xbd,0x00,0x20,0x30,0x60,0xa4,0x48,
    0xff,0xf7,0x0a,0xfc,0x04,0x00,0x1a,0xd0,0xa0,0x1c,0x18,0xd0,0x1f,0x20,0xa8,0x70,
    0x30,0x00,0x27,0xf5,0x58,0xfa,0x06,0x00,0x28,0x00,0x27,0xf5,0x54,0xfa,0x05,0x00,
    0x20,0x00,0x27,0xf5,0x50,0xfa,0x02,0x22,0x02,0x92,0x02,0x00,0x9a,0x48,0x01,0x96,
    0x00,0x95,0x7c,0x68,0x1f,0x21,0x00,0x68,0x00,0x23,0xa0,0x47,0xfe,0xbd,0x00,0x20,
    0xc0,0x43,0xfe,0xbd,
};
static const uint8_t wrapper_calls[] = {32,50,58,66};
static const uint8_t wrapper_literals[] = {4,6,30,76};
static const uint8_t periodic_code[] = {
    0xf8,0xb5,0x08,0x20,0x25,0xf1,0x54,0xfa,0x06,0x00,0x08,0x20,0x25,0xf1,0x50,0xfa,
    0x27,0x4d,0x04,0x00,0x69,0x88,0x2e,0x48,0xb1,0x80,0xb1,0x88,0x40,0x1c,0x8e,0xf0,
    0x74,0xfe,0x63,0x20,0x25,0xf1,0xeb,0xff,0x21,0x00,0x30,0x00,0x10,0xf0,0x07,0xff,
    0x01,0x00,0x1d,0x48,0x8e,0xf0,0x69,0xfe,0x1b,0x48,0x21,0x68,0x80,0x1d,0x8e,0xf0,
    0x64,0xfe,0x63,0x20,0x26,0xf1,0xb8,0xf8,0xe1,0x88,0x21,0x48,0x69,0x80,0x8e,0xf0,
    0x5c,0xfe,0x20,0x68,0x01,0x28,0x20,0xd1,0xa0,0x88,0x1d,0x4f,0xbf,0x1c,0x1e,0x28,
    0x0d,0xd9,0x7d,0x23,0x1e,0x38,0xdb,0x00,0x58,0x43,0x05,0x21,0x4a,0xf1,0x24,0xef,
    0x01,0x00,0x28,0x88,0x21,0xf1,0x5c,0xfb,0xa1,0x88,0x1e,0x39,0x0a,0xe0,0x7d,0x23,
    0xdb,0x00,0x58,0x43,0x05,0x21,0x3d,0xf1,0xa0,0xeb,0x01,0x00,0x28,0x88,0x21,0xf1,
    0x4f,0xfb,0xa1,0x88,0x38,0x00,0x8e,0xf0,0x38,0xfe,0x20,0x00,0x25,0xf1,0xd7,0xfc,
    0x30,0x00,0x25,0xf1,0xd4,0xfc,0xf8,0xbd,
};
static const uint8_t periodic_calls[] = {4,12,30,36,44,52,62,68,78,108,116,134,142,150,156,162};
static const uint8_t periodic_literals[] = {16,22,50,56,74,90};
static const uint8_t lookup_code[] = {
    0x70,0xb5,0x8b,0x4d,0x06,0x00,0x2c,0x68,0x00,0x2c,0x0d,0xd1,0xa0,0x1e,0x70,0xbd,
    0x21,0x00,0x14,0x31,0x30,0x00,0xa1,0xf1,0x40,0xeb,0x00,0x28,0x03,0xd1,0x20,0x68,
    0x29,0x68,0x40,0x18,0x70,0xbd,0x20,0x34,0x20,0x7d,0xff,0x28,0xf0,0xd1,0x00,0x20,
    0x70,0xbd,
};
static const uint8_t lookup_calls[] = {22};
static const uint8_t lookup_literals[] = {2};
static const uint8_t init_code[] = {
    0xfe,0xb5,0x04,0x00,0xed,0x48,0xed,0x4d,0x00,0x29,0x68,0x60,0x01,0xd0,0x01,0x20,
    0x08,0x60,0x04,0x20,0xc0,0x43,0x00,0x2c,0x20,0xd0,0x21,0x69,0x00,0x29,0x1d,0xd0,
    0xeb,0x48,0x29,0x60,0xe0,0x38,0x27,0xf5,0x08,0xff,0xe9,0x49,0x08,0x60,0xa0,0x68,
    0x27,0xf5,0x03,0xff,0x06,0x00,0xe5,0x48,0xd0,0x38,0x27,0xf5,0xfe,0xfe,0xe2,0x68,
    0x33,0x00,0x00,0x92,0x62,0x68,0x6c,0x68,0x1f,0x21,0xa0,0x47,0x01,0x28,0x05,0xd1,
    0x00,0x20,0x01,0x00,0x02,0x90,0x01,0xa8,0xff,0xf7,0xaf,0xff,0xfe,0xbd,
};
static const uint8_t init_calls[] = {38,48,58,88};
static const uint8_t init_literals[] = {4,6,32,42,54};
static const uint8_t registration_code[] = {
    0x00,0x20,0x68,0x61,0xe2,0xa0,0x3e,0xf5,0x82,0xf9,0x28,0x61,0xe3,0x48,0x68,0x60,
    0xe0,0x6f,0xa8,0x60,0xdc,0x48,0x00,0x68,0xe8,0x60,0x63,0x20,0x2a,0xf1,0x9a,0xfe,
    0x05,0x99,0x28,0x00,0x15,0xf0,0x0c,0xf9,
};
static const uint8_t registration_calls[] = {6,28,36};
static const uint8_t registration_literals[] = {4,12,20};
static const uint8_t callback_code[] = {
    0xfe,0xb5,0x0d,0x00,0x04,0x00,0x01,0x00,0x89,0x20,0x00,0x27,0x00,0x04,0x93,0xf0,
    0x7f,0xff,0xf4,0x48,0x00,0x26,0x84,0x30,0x01,0x2c,0x26,0xd0,0x02,0x2c,0x3f,0xd0,
    0x03,0x2c,0x20,0xd1,0xf3,0x4d,0x28,0x68,0x2a,0xf1,0x45,0xfb,0xee,0x4c,0x01,0x90,
    0x08,0x3c,0x60,0x68,0x2a,0x68,0xc1,0x6f,0x01,0x98,0x44,0xf1,0x00,0xe8,0x01,0x20,
    0x60,0x70,0x00,0x22,0x01,0x00,0x00,0x92,0xff,0x20,0x2b,0x68,0x01,0x9a,0x35,0x30,
    0x51,0xf1,0xe5,0xff,0x00,0x28,0x06,0xd0,0x61,0x68,0x80,0x31,0x0e,0x70,0x01,0x00,
    0xe5,0x48,0x93,0xf0,0x55,0xff,0x38,0x00,0xfe,0xbd,0x00,0x24,0x07,0x00,0x04,0xe0,
    0x64,0x1c,0x24,0x06,0x24,0x0e,0x04,0x2c,0x0f,0xd2,0xe0,0x00,0x38,0x58,0x00,0x28,
    0xf6,0xd1,0x04,0x2c,0x09,0xd2,0x28,0x00,0x2a,0xf1,0x15,0xfb,0xe4,0x00,0x38,0x51,
    0x3d,0xf5,0x4b,0xfa,0xe1,0x19,0x48,0x60,0x00,0xe0,0x30,0x00,0x07,0x00,0xe2,0xe7,
    0x00,0x21,0x00,0x2d,0xdf,0xd0,0x04,0xe0,0x49,0x1c,0x09,0x06,0x09,0x0e,0x04,0x29,
    0x0f,0xd2,0xca,0x00,0x12,0x18,0x52,0x68,0xaa,0x42,0xf5,0xd1,0x04,0x29,0x08,0xd2,
    0xcc,0x00,0x05,0x00,0x00,0x59,0x2a,0xf1,0xc5,0xfd,0x60,0x19,0x2e,0x51,0x46,0x60,
    0xc9,0xe7,0xc8,0x48,0x40,0x1d,0xd0,0xf0,0x9d,0xfe,0xc4,0xe7,
};
static const uint8_t callback_calls[] = {14,40,58,80,98,136,144,198,214};
static const uint8_t callback_literals[] = {18,36,44,96,210,8,12};

static const uint8_t pool_header_code[] = {
    0x30,0x88,0x75,0x68,0x80,0x21,0x40,0x1e,0x30,0x80,0x70,0x68,0x06,0x9b,0x02,0x68,
    0x4b,0x40,0x06,0x99,0x0c,0x9f,0x72,0x60,0xac,0x48,0xeb,0x70,0x2f,0x80,0xc0,0x78,
    0xaa,0x4a,0x49,0x00,0x68,0x32,0xa8,0x70,0x50,0x5a,0x40,0x1c,0x00,0x04,0x00,0x0c,
    0x50,0x52,0x1e,0x32,0x53,0x5a,0x98,0x42,0x00,0xd9,0x50,0x52,0x04,0x99,0x03,0x98,
    0xa9,0x60,0x68,0x60,0x28,0x00,0x0c,0x30,0x08,0x90,
};
static const uint8_t pool_header_calls[] = {};
static const uint8_t pool_header_literals[] = {24,32};
static const uint8_t heap_header_code[] = {
    0x78,0x42,0x60,0x60,0xcb,0xf0,0xff,0xfa,0xbe,0x00,0x37,0x19,0x20,0x72,0x29,0x00,
    0x38,0x00,0xff,0xf7,0xe3,0xfe,0x00,0x28,0x01,0xd0,0x00,0x20,0xf8,0x60,0xe8,0x6a,
    0x80,0x19,0xe8,0x62,0xa9,0x8f,0x49,0x1c,0x09,0x04,0x09,0x0c,0xa9,0x87,0x6a,0x69,
    0x82,0x42,0x00,0xd2,0x68,0x61,0xa8,0x8c,0x88,0x42,0x00,0xd2,0xa9,0x84,0xa8,0x69,
    0xb0,0x42,0x00,0xd2,0xae,0x61,0x10,0x34,0x07,0xe0,
};
static const uint8_t heap_header_calls[] = {4,18};
static const uint8_t heap_header_literals[] = {};

static int match(const uint8_t *p, const uint8_t *pattern, unsigned length,
                 const uint8_t *calls, unsigned ncalls,
                 const uint8_t *literals, unsigned nliterals)
{
    for (unsigned i = 0; i < length; ++i) {
        int masked = 0;
        for (unsigned j = 0; j < ncalls; ++j)
            if (i >= calls[j] && i < calls[j] + 4u) masked = 1;
        for (unsigned j = 0; j < nliterals; ++j)
            if (i == literals[j]) masked = 1;
        if (!masked && p[i] != pattern[i]) return 0;
    }
    for (unsigned j = 0; j < ncalls; ++j) {
        unsigned i = calls[j];
        if ((half(p + i) & 0xf800) != 0xf000 ||
            (half(p + i + 2) & 0xf800) != (half(pattern + i + 2) & 0xf800))
            return 0;
    }
    return 1;
}
#define MATCH(p, name) match(p, name##_code, sizeof(name##_code), \
    name##_calls, sizeof(name##_calls), name##_literals, sizeof(name##_literals))

static uint32_t target(const uint8_t *rom, uint32_t offset)
{
    uint32_t a = half(rom + offset), b = half(rom + offset + 2);
    int32_t delta = ((a & 2047) << 12) | ((b & 2047) << 1);
    if (delta & 0x400000) delta -= 0x800000;
    return offset + 4 + (uint32_t)delta;
}
static uint32_t literal(const uint8_t *rom, uint32_t bytes, uint32_t offset)
{
    uint32_t address = ((offset + 4) & ~3u) + ((uint32_t)rom[offset] << 2);
    return address <= bytes - 4 ? word(rom + address) : 0;
}
static int string_at(const uint8_t *rom, uint32_t base, uint32_t bytes,
                     uint32_t address, const char *text)
{
    unsigned i = 0;
    if (address < base) return 0;
    address -= base;
    do {
        if (address >= bytes || rom[address++] != (uint8_t)text[i]) return 0;
    } while (text[i++]);
    return 1;
}
static int ram_va(uint32_t va) { return va >= 0xd8000000u && va < 0xda000000u; }

int linux6303_swd_discover(const uint8_t *rom, uint32_t base, uint32_t bytes,
                         struct linux6303_swd_symbols *out)
{
    uint32_t wrapper = 0, periodic = 0, init = 0, registration = 0;
    struct linux6303_swd_symbols s = {0};
    if (!rom || !out || (base & 3) || bytes < 1024 || bytes > 0xffffffffu - base)
        return 0;
    *out = s;
    for (uint32_t off = 0; off <= bytes - 1024; off += 2) {
        if (rom[off] != 0xfe || rom[off + 1] != 0xb5 || !MATCH(rom + off, wrapper))
            continue;
        uint32_t lookup = target(rom, off + 32);
        if (lookup > bytes - sizeof(lookup_code) || !MATCH(rom + lookup, lookup) ||
            literal(rom, bytes, off + 4) != 0xffff0809u ||
            literal(rom, bytes, lookup + 2) != literal(rom, bytes, off + 6) ||
            !string_at(rom, base, bytes, literal(rom, bytes, off + 30), "PA_SWD") ||
            target(rom, off + 50) != target(rom, off + 58) ||
            target(rom, off + 50) != target(rom, off + 66)) continue;
        if (wrapper) return 0;
        wrapper = off;
    }
    if (!wrapper) return 0;
    s.wrapper_va = base + wrapper;
    s.rom_entry = literal(rom, bytes, wrapper + 4);
    s.table_global_va = literal(rom, bytes, wrapper + 6);
    s.descriptor_global_va = literal(rom, bytes, wrapper + 76);
    if (!ram_va(s.table_global_va) || (s.table_global_va & 3) ||
        !ram_va(s.descriptor_global_va) || (s.descriptor_global_va & 3)) return 0;
    for (uint32_t off = 0; off <= bytes - 1024; off += 2) {
        if (rom[off] == 0xf8 && rom[off + 1] == 0xb5 &&
            MATCH(rom + off, periodic) && target(rom, off + 44) == wrapper) {
            if (periodic) return 0;
            periodic = off;
        }
        if (rom[off] == 0xfe && rom[off + 1] == 0xb5 &&
            MATCH(rom + off, init) &&
            literal(rom, bytes, off + 4) == s.rom_entry &&
            literal(rom, bytes, off + 6) == s.table_global_va &&
            literal(rom, bytes, off + 42) == s.descriptor_global_va) {
            if (init) return 0;
            init = off;
        }
    }
    if (!periodic || !init) return 0;
    s.token_va = literal(rom, bytes, periodic + 16) + 2;
    s.descriptor_va = literal(rom, bytes, init + 32) - 0xe0;
    if (!ram_va(s.token_va) || (s.token_va & 1) ||
        s.descriptor_va < base || s.descriptor_va >= 0xf4000000u ||
        (s.descriptor_va & 3)) return 0;
    unsigned pool_headers = 0, heap_headers = 0;
    uint32_t allocator = target(rom, periodic + 4);
    if (target(rom, periodic + 12) != allocator) return 0;
    for (uint32_t off = 0; off <= bytes - 1024; off += 2) {
        if (off >= allocator && off - allocator < 4096 &&
            rom[off] == pool_header_code[0] && MATCH(rom + off, pool_header)) ++pool_headers;
        if (rom[off] == heap_header_code[0] && MATCH(rom + off, heap_header)) ++heap_headers;
        if (rom[off] != 0 || rom[off + 1] != 0x20 ||
            !MATCH(rom + off, registration) || target(rom, off + 36) != init)
            continue;
        uint32_t callback_va = literal(rom, bytes, off + 12);
        uint32_t c = (callback_va & ~1u) - base;
        if (!(callback_va & 1) || c > bytes - sizeof(callback_code) ||
            !MATCH(rom + c, callback) || target(rom, c + 136) != allocator ||
            literal(rom, bytes, c + 18) != literal(rom, bytes, c + 44) ||
            literal(rom, bytes, off + 20) != literal(rom, bytes, c + 36)) continue;
        if (registration) return 0;
        registration = off;
        s.callback_va = callback_va;
        s.server_global_va = literal(rom, bytes, c + 44) - 4;
        s.allocation_table_va = literal(rom, bytes, c + 18) + 0x84;
        s.work_bytes_va = literal(rom, bytes, off + 20);
    }
    if (!registration || pool_headers != 1 || heap_headers != 1 ||
        !ram_va(s.server_global_va) || (s.server_global_va & 3) ||
        !ram_va(s.allocation_table_va) || (s.allocation_table_va & 3) ||
        !ram_va(s.work_bytes_va) || (s.work_bytes_va & 3)) return 0;
    *out = s;
    return 1;
}

#ifndef LINUX6303_SWD_SCAN_ONLY
static int translated(const struct mmu_cpu *cpu, uint32_t va, uint32_t bytes,
                      uint32_t *pa, int ram)
{
    struct mmu_range r;
    linux6303_translate_mapping(cpu, va, bytes, &r);
    if (r.status) return 0;
    if (ram ? (r.pa < 0x10000000u || bytes > 0x12000000u - r.pa || r.pa >= 0x12000000u)
            : (r.pa >= 0x04000000u || bytes > 0x04000000u - r.pa)) return 0;
    *pa = r.pa;
    return 1;
}

/* S40 allocator layouts are verified by discovery above. The fixed-pool
 * header records the requested length; the heap header records full capacity.
 * Linux keeps payload bytes only: it never calls the retired S40 deallocator. */
static uint32_t allocation_bytes(const struct mmu_cpu *cpu, uint32_t va, uint32_t expected_pa)
{
    uint32_t pa, bytes;
    if ((va & 3) || va < 16 || !expected_pa ||
        !translated(cpu, va - 16, 16, &pa, 1)) return 0;
    const volatile uint32_t *header = (void *)(uintptr_t)(va - 16);
    if (header[0] == 0xd1cec0deu) {
        int32_t words = (int32_t)header[1];
        if (words >= -5 || words < -(0x10000 / 4 + 5)) return 0;
        bytes = (uint32_t)(-words) * 4 - 20;
        if (!translated(cpu, va, bytes + 4, &pa, 1) || pa != expected_pa ||
            *(const volatile uint32_t *)(uintptr_t)(va + bytes) != 0x31415927u) return 0;
    } else {
        uint32_t tag = *(const volatile uint32_t *)(uintptr_t)(va - 12);
        if (((tag >> 24) ^ 0x80u) >= 15) return 0;
        bytes = tag & 0xffff;
        if (!bytes || !translated(cpu, va, bytes, &pa, 1) || pa != expected_pa) return 0;
    }
    return bytes;
}

int linux6303_swd_refresh_token(const struct mmu_cpu *cpu,
                              const struct linux6303_swd_symbols *s,
                              struct rm443_swd_state *out)
{
    uint32_t pa;
    if (!translated(cpu, s->token_va, 2, &pa, 1) ||
        !translated(cpu, s->server_global_va, 4, &pa, 1) ||
        !translated(cpu, s->work_bytes_va, 4, &pa, 1) ||
        !translated(cpu, s->allocation_table_va, 32, &pa, 1)) return 1;
    uint32_t server = *(const volatile uint32_t *)(uintptr_t)s->server_global_va;
    if (server > 0xffffffffu - 0x80 || !translated(cpu, server + 0x7c, 4, &pa, 1)) return 2;
    uint32_t work_va = *(const volatile uint32_t *)(uintptr_t)(server + 0x7c);
    if (*(const volatile uint32_t *)(uintptr_t)s->work_bytes_va != out->work_bytes ||
        !translated(cpu, work_va, out->work_bytes, &pa, 1) || pa != out->work_pa) return 3;
    for (unsigned i = 0; i < 4; ++i) {
        const volatile uint32_t *slot = (void *)(uintptr_t)(s->allocation_table_va + i * 8);
        if (slot[0] != out->firmware_alloc_va[i] || slot[1] != out->firmware_alloc_pa[i]) return 4;
        if (slot[0] && allocation_bytes(cpu, slot[0], slot[1]) != out->firmware_alloc_bytes[i]) return 5;
    }
    out->token = *(const volatile uint16_t *)(uintptr_t)s->token_va;
    return 0;
}

static int capture_inner(const struct mmu_cpu *cpu,
                        const struct linux6303_swd_symbols *s,
                        struct rm443_swd_state *out)
{
    uint32_t pa;
    static const uint8_t descriptor[16] = {
        0xab,0x0f,0xbd,0x96,0xaf,0x2a,0xc2,0x71,0xd2,0x62,0x64,0xaf,0,0,0x12,0
    };
    if (!translated(cpu, s->descriptor_va, sizeof(descriptor), &out->descriptor_pa, 0) ||
        !translated(cpu, s->descriptor_global_va, 4, &pa, 1)) return 1;
    const volatile uint8_t *desc = (void *)(uintptr_t)s->descriptor_va;
    for (unsigned i = 0; i < sizeof(descriptor); ++i)
        if (desc[i] != descriptor[i]) return 2;
    if (*(const volatile uint32_t *)(uintptr_t)s->descriptor_global_va != out->descriptor_pa)
        return 3;
    if (!translated(cpu, s->table_global_va, 4, &pa, 1)) return 4;
    uint32_t table = *(const volatile uint32_t *)(uintptr_t)s->table_global_va;
    if (!translated(cpu, table, 32, &pa, 0)) return 5;
    unsigned found = 0, terminated = 0;
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t address = table + i * 32;
        if (address < table || !translated(cpu, address, 32, &pa, 0)) return 6;
        const volatile uint8_t *entry = (void *)(uintptr_t)address;
        if (entry[20] == 0xff) { terminated = 1; break; }
        const char name[] = "PA_SWD";
        unsigned n;
        for (n = 0; n < sizeof(name); ++n) if (entry[20 + n] != name[n]) break;
        if (n != sizeof(name)) continue;
        if (found++) return 7;
        uint32_t offset = *(const volatile uint32_t *)(uintptr_t)address;
        out->pa_header_bytes = *(const volatile uint32_t *)(uintptr_t)(address + 4);
        if (offset > 0x100000u || !out->pa_header_bytes || out->pa_header_bytes > 0x10000u ||
            table + offset < table || !translated(cpu, table + offset,
                out->pa_header_bytes, &out->pa_header_pa, 0)) return 8;
    }
    if (!found || !terminated) return 9;
    if (!translated(cpu, s->work_bytes_va, 4, &pa, 1) ||
        !translated(cpu, s->server_global_va, 4, &pa, 1) ||
        !translated(cpu, s->allocation_table_va, 32, &pa, 1)) return 10;
    out->work_bytes = *(const volatile uint32_t *)(uintptr_t)s->work_bytes_va;
    if (!out->work_bytes || out->work_bytes > 0x10000u) return 11;
    uint32_t server = *(const volatile uint32_t *)(uintptr_t)s->server_global_va;
    if (server > 0xffffffffu - 0x80 || !translated(cpu, server + 0x7c, 4, &pa, 1)) return 12;
    uint32_t work_va = *(const volatile uint32_t *)(uintptr_t)(server + 0x7c);
    if (!translated(cpu, work_va, out->work_bytes, &out->work_pa, 1)) return 13;
    for (unsigned i = 0; i < 4; ++i) {
        const volatile uint32_t *slot = (void *)(uintptr_t)(s->allocation_table_va + i * 8);
        out->firmware_alloc_va[i] = slot[0];
        out->firmware_alloc_pa[i] = slot[1];
        if (!slot[0] && !slot[1]) continue;
        out->firmware_alloc_bytes[i] = allocation_bytes(cpu, slot[0], slot[1]);
        if (!out->firmware_alloc_bytes[i]) return 14;
    }
    out->rom_entry = s->rom_entry;
    out->callback_va = s->callback_va;
    out->firmware_sctlr = cpu->control;
    return linux6303_swd_refresh_token(cpu, s, out) ? 15 : 0;
}

int linux6303_swd_capture(const struct mmu_cpu *cpu,
                        const struct linux6303_swd_symbols *symbols,
                        struct rm443_swd_state *out)
{
    *out = (struct rm443_swd_state){ .magic = RM443_SWD_MAGIC,
                                   .version = RM443_SWD_VERSION };
    out->capture_status = capture_inner(cpu, symbols, out);
    return out->capture_status;
}

unsigned linux6303_swd_map(const struct mmu_cpu *cpu, uint32_t va_base,
                         uint32_t fine[1024])
{
    unsigned mapped = 0;
    if (va_base & 0xfffff) return 0;
    for (unsigned i = 0; i < 1024; ++i) {
        struct mmu_range r;
        uint32_t va = va_base + i * 1024;
        /* translate_range rejects an exclusive end of 2^32; translating the
         * first1023 bytes still proves this aligned1KiB mapping. */
        linux6303_translate_mapping(cpu, va, 1023, &r);
        fine[i] = 0;
        if (!r.status && !(r.pa & 1023)) {
            fine[i] = r.pa | 0x13u; /* tiny page, privileged RW, no cache */
            ++mapped;
        }
    }
    /* ROM entry must retain its physical destination; the SoC alias must
     * include the watchdog block. These distinguish a usable capture from
     * unrelated valid descriptors elsewhere in the same megabyte. */
    if (va_base == 0xfff00000u &&
        (fine[0xf0800 / 1024] & ~1023u) != 0xffff0800u) return 0;
    if (va_base == 0xffd00000u &&
        (fine[0x40400 / 1024] & ~1023u) != 0x20040400u) return 0;
    return mapped;
}
#endif
