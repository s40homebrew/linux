/* SPDX-License-Identifier: MIT */
#ifndef LINUX6303_SECURE_WATCHDOG_H
#define LINUX6303_SECURE_WATCHDOG_H
#include <stdint.h>
#include "../../kernel/overlay/arch/arm/mach-nokia-raps/rm443_secure_wdt_abi.h"
struct mmu_cpu;

#define LINUX6303_SWD_ROM_BASE 0xf0100400u
#define LINUX6303_SWD_ROM_BYTES 0x00400000u

struct linux6303_swd_symbols {
    uint32_t wrapper_va, rom_entry, descriptor_va, descriptor_global_va;
    uint32_t table_global_va, token_va, callback_va, allocation_table_va;
    uint32_t server_global_va, work_bytes_va;
};

/* Pure bounded signature parser. Returns one only for an unambiguous contract. */
int linux6303_swd_discover(const uint8_t *rom, uint32_t base, uint32_t bytes,
                         struct linux6303_swd_symbols *out);

/* No secure calls or writes. Caller validates the ARM926 table walker first.
 * Returns zero on success; reserve work_pa..work_pa+work_bytes in place.
 * Existing callback allocations are reported, never silently reclaimed. */
int linux6303_swd_capture(const struct mmu_cpu *cpu,
                        const struct linux6303_swd_symbols *symbols,
                        struct rm443_swd_state *out);
/* Call with IRQ/FIQ already masked after the final firmware sleep/checkpoint,
 * then enter the one-way handoff without allowing another S40 timer callback. */
int linux6303_swd_refresh_token(const struct mmu_cpu *cpu,
                              const struct linux6303_swd_symbols *symbols,
                              struct rm443_swd_state *out);
/* Copy one original 1 MiB mapping as ARMv5 fine-table descriptors, preserving
 * holes and physical destinations while normalizing privileged RW, uncached.
 * Returns mapped 1 KiB pages, or zero if the required ROM/SoC mapping is absent. Does not read any peripheral/ROM contents. */
unsigned linux6303_swd_map(const struct mmu_cpu *cpu, uint32_t va_base,
                         uint32_t fine[1024]);
#endif
