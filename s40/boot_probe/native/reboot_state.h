/* SPDX-License-Identifier: MIT */
#ifndef LINUX6303_REBOOT_STATE_H
#define LINUX6303_REBOOT_STATE_H
#include <stdint.h>
struct mmu_cpu;

/* Audited RM-443 ROM window, shared by 08.90 and 10.12. No code is called. */
#define LINUX6303_REBOOT_ROM_BASE 0xf0100400u
#define LINUX6303_REBOOT_ROM_BYTES 0x00400000u
#define LINUX6303_REBOOT_TOKEN_OFFSET 0x124u
#define LINUX6303_REBOOT_TOKEN 0x0a9ef561u

/* Pure bounded parser, also exercised against the local firmware fixtures. */
uint32_t linux6303_reboot_reason_va(const uint8_t *rom, uint32_t base,
                                  uint32_t bytes);
/* Caller has validated CPU and the existing page-table walker. Returns zero
 * unless ROM semantics, live retention marker and a full DDR page are checked.
 * Reserve that physical page before Linux can allocate it. */
uint32_t linux6303_reboot_reason_pa(const struct mmu_cpu *cpu);
#endif
