/* SPDX-License-Identifier: MIT */
#ifndef LINUX6303_RELOCATION_ABI_H
#define LINUX6303_RELOCATION_ABI_H
#define RELOC_SOURCE 0
#define RELOC_DESTINATION 4
#define RELOC_IMAGE_BYTES 8
#define RELOC_MEMORY_BYTES 12
#define RELOC_DTB 16
#define RELOC_TRACE 20
#define RELOC_QUIESCE_STATUS 24
#define RELOC_DSP_BEFORE 28
#define RELOC_DSP_AFTER 32
#define RELOC_QUIESCE_DONE 36
#define RELOC_LCD_VA 40
#define RELOC_EXPECTED_HASH 44
#define TRACE_STAGE 12
#define TRACE_QUIESCE_STATUS 68
#define TRACE_COPIED_BYTES 72
#define TRACE_RELOCATION_FLAGS 76
#define TRACE_DSP_BEFORE 80
#define TRACE_DSP_AFTER 84
#define TRACE_QUIESCE_DONE 88
#define TRACE_ENTRY_CPSR 92
#define TRACE_ENTRY_SCTLR 96
#define TRACE_ENTRY_TTBR 100
#define TRACE_ENTRY_DACR 104
#define TRACE_ENTRY_FCSE 108
#define TRACE_EXPECTED_HASH 112
#define TRACE_NATIVE_HASH 116
#define TRACE_SOURCE_HASH 120
#define TRACE_DEST_HASH 124
#ifndef __ASSEMBLER__
#include <stdint.h>
/* Never on the old firmware stack: both records live in retained scratch. */
struct linux6303_relocation {
    uint32_t source, destination, image_bytes, memory_bytes, dtb, trace;
    uint32_t quiesce_status, dsp_before, dsp_after, quiesce_done;
    uint32_t lcd_va; /* verified uncached alias; zero disables pre-MMU markers */
    uint32_t expected_hash;
};
struct linux6303_boot_trace {
    /* Preserve existing capture tools' first fourteen words. */
    uint32_t magic0, magic1, format, stage, base_pa, bytes, log_pa, image_pa;
    uint32_t stub_va, stub_pa, ttbr_pa, dtb_pa, old_ttbr, old_domains;
    uint32_t source_pa, scratch_pa, scratch_bytes, quiesce_status, copied_bytes;
    uint32_t relocation_flags, dsp_before, dsp_after, quiesce_done;
    uint32_t entry_cpsr, entry_sctlr, entry_ttbr, entry_dacr, entry_fcse;
    uint32_t expected_hash, native_hash, source_hash, destination_hash;
};
_Static_assert(sizeof(struct linux6303_relocation) == 48, "relocation ABI");
_Static_assert(sizeof(struct linux6303_boot_trace) == 128, "retained trace ABI");
#endif
#endif
