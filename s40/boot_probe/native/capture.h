/* SPDX-License-Identifier: MIT */
#ifndef LINUX6303_CAPTURE_H
#define LINUX6303_CAPTURE_H
#include <stdint.h>
#define MMU_TARGETS 6
struct mmu_cpu {
    uint32_t cpsr, midr, cache_type, control, ttbr, domains, fcse, pc, sp;
};
struct mmu_target {
    uint32_t va, l1, l2, pa, status, table_pa, table_bytes;
    uint32_t table[1024];
};
struct mmu_snapshot {
    struct mmu_cpu cpu;
    uint32_t walker_va, status, cpsr_after, domains_after;
    uint32_t l1[4096];
    struct mmu_target targets[MMU_TARGETS];
};
struct mmu_range {
    uint32_t va, bytes, pa, checked_bytes, first_bad_va, status;
};
int linux6303_mmu_cpu(struct mmu_cpu *cpu);
uint32_t linux6303_find_table_walker(void);
void linux6303_capture(struct mmu_snapshot *snapshot);
/* Caller validates CPU and find_table_walker(), and owns the whole allocation.
 * A zero status proves translation/contiguity only, not ownership or DMA safety. */
void linux6303_translate_range(const struct mmu_cpu *cpu, uint32_t va,
                              uint32_t bytes, struct mmu_range *result);
/* Same audited page-table walk, but permits NOR/ROM/MMIO destinations. Only
 * page-table DDR is read; the translated target is never dereferenced. Callers
 * must validate the resulting physical range before reading target memory. */
void linux6303_translate_mapping(const struct mmu_cpu *cpu, uint32_t va,
                                uint32_t bytes, struct mmu_range *result);
/* Find an existing uncached/unbuffered privileged-RW alias for one MMIO word.
 * Caller validates CPU and find_table_walker() first. Only table RAM is read;
 * the peripheral is never accessed. Zero means no supported usable mapping.
 * Supports sections and coarse small pages used by RM-443 firmware. */
uint32_t linux6303_mmio_alias(const struct mmu_cpu *cpu, uint32_t expected_pa);
#endif
