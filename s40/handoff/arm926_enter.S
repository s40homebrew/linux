/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARM926 transition from S40 to Linux.
 *
 * Caller must stop DMA/modem writers, service watchdogs, validate physical
 * regions, and initialize/synchronize this code and the
 * new page tables. Entry in ARM System or SVC mode, FCSE=0, little-endian.
 * New tables map this entire code page at BOTH the current VA and physical VA.
 * No stack/data access occurs after changing mode or translation tables.
 *
 * r0: ARM kernel physical entry; r1: DTB physical address (8-byte aligned)
 * r2: physical L1 base (16 KiB aligned), domain zero mappings
 * r3: physical address of arm926_identity (NOT the code page base)
 * Returns -1 without modifying control state for basic precondition failures.
 * Success is one-way, clobbers r4-r11, switches to SVC with IRQ/FIQ masked.
 *
 * Cache maintenance follows the ARM926 operation documented by ARM and used
 * in Linux arch/arm/mm/proc-arm926.S; do not substitute ARMv6 cpsid/dsb/isb.
 */
.syntax unified
.arch armv5te
.arm
.section .text.entry,"ax",%progbits
.balign 32
.global arm926_enter
.type arm926_enter,%function
arm926_enter:
    /* Until all checks pass, preserve AAPCS callee-saved registers. */
    mrs ip, cpsr
    and ip, ip, #31
    cmp ip, #0x1f
    cmpne ip, #0x13
    bne .Lrefuse
    tst r0, #3
    bne .Lrefuse
    tst r1, #7
    bne .Lrefuse
    mov ip, r2, lsl #18
    cmp ip, #0
    bne .Lrefuse
    tst r3, #3
    bne .Lrefuse
    mrc p15, 0, ip, c13, c0, 0
    cmp ip, #0
    bne .Lrefuse
    mrc p15, 0, ip, c1, c0, 0
    tst ip, #128                 /* ARMv5 B bit: must be little-endian */
    bne .Lrefuse
    /* MIDR check: implementer ARM, architecture 5TEJ, part ARM926.
     * Shift discards revision, separately remove variant [23:20]. */
    mrc p15, 0, ip, c0, c0, 0
    bic ip, ip, #0x00f00000
    bic ip, ip, #15
    eor ip, ip, #0x41000000
    eor ip, ip, #0x00069000
    eors ip, ip, #0x260
    bne .Lrefuse
    mov r8, r0
    mov r9, r1
    mov r10, r2
    mov r11, r3
    mov ip, #0xd3
    msr cpsr_c, ip
.Lclean:
    mrc p15, 0, apsr_nzcv, c7, c14, 3  /* test, clean and invalidate D-cache */
    bne .Lclean
    mov r4, #0
    mcr p15, 0, r4, c7, c10, 4  /* drain write buffer */
    mrc p15, 0, r5, c1, c0, 0
    bic r5, r5, #0x1000         /* I-cache off */
    bic r5, r5, #0x000e         /* D-cache, write buffer, alignment trap off */
    mcr p15, 0, r5, c1, c0, 0
    mcr p15, 0, r4, c7, c5, 0  /* invalidate I-cache */
    mrc p15, 0, r6, c3, c0, 0
    orr r6, r6, #3             /* retain old domains until new TTBR is active */
    mcr p15, 0, r6, c3, c0, 0
    mcr p15, 0, r10, c2, c0, 0
    mcr p15, 0, r4, c8, c7, 0
    bx r11
.global arm926_identity
arm926_identity:
    mov r6, #1
    mcr p15, 0, r6, c3, c0, 0  /* now domain 0 client, others fault */
    bic r5, r5, #1
    mcr p15, 0, r5, c1, c0, 0  /* same PA and VA across MMU disable */
    nop
    nop
    nop
    mcr p15, 0, r4, c8, c7, 0
    mov r0, #0
    mvn r1, #0
    mov r2, r9
    bx r8
.Lrefuse:
    mvn r0, #0
    bx lr
.size arm926_enter, .-arm926_enter

/* Full-DDR path. Plan is trusted validated caller memory; no old stack or
 * firmware pointer is used after the first irreversible CPU-state change.
 * r0=plan VA, r1=plan PA, r2=private TTBR PA, r3=relocate_identity PA.
 */
#include "relocation_abi.h"
.balign 32
.global arm926_relocate
.type arm926_relocate,%function
arm926_relocate:
    mrs ip, cpsr
    and ip, ip, #31
    cmp ip, #0x1f
    cmpne ip, #0x13
    bne .Lreloc_refuse
    orr ip, r0, r1
    orr ip, ip, r3
    tst ip, #3
    bne .Lreloc_refuse
    mov ip, r2, lsl #18
    cmp ip, #0
    bne .Lreloc_refuse
    mrc p15, 0, ip, c13, c0, 0
    cmp ip, #0
    bne .Lreloc_refuse
    mrc p15, 0, ip, c1, c0, 0
    tst ip, #128
    bne .Lreloc_refuse
    mrc p15, 0, ip, c0, c0, 0
    bic ip, ip, #0x00f00000
    bic ip, ip, #15
    eor ip, ip, #0x41000000
    eor ip, ip, #0x00069000
    eors ip, ip, #0x260
    bne .Lreloc_refuse
    mov r8, r1
    ldr r9, [r0, #RELOC_LCD_VA]
    mov r10, r2
    mov r11, r3
    mov ip, #0xd3
    msr cpsr_c, ip
    cmp r9, #0
    beq .Lreloc_clean
    mov r0, #0x37             /* IRQ/FIQ masked, old tables/caches still active */
    mov r1, r9
    bl rm443_boot_marker_at
.Lreloc_clean:
    mrc p15, 0, apsr_nzcv, c7, c14, 3
    bne .Lreloc_clean
    mov r4, #0
    mcr p15, 0, r4, c7, c10, 4
    mrc p15, 0, r5, c1, c0, 0
    bic r5, r5, #0x1000
    bic r5, r5, #0x000e
    mcr p15, 0, r5, c1, c0, 0
    mcr p15, 0, r4, c7, c5, 0
    cmp r9, #0
    beq .Lreloc_tables
    mov r0, #0x38             /* cache clean completed; caches now off */
    mov r1, r9
    bl rm443_boot_marker_at
.Lreloc_tables:
    mov r4, #0               /* marker clobbers r0-r7/ip */
    mrc p15, 0, r6, c3, c0, 0
    orr r6, r6, #3
    mcr p15, 0, r6, c3, c0, 0
    mcr p15, 0, r10, c2, c0, 0
    mcr p15, 0, r4, c8, c7, 0
    cmp r9, #0
    beq .Lreloc_identity_branch
    mov r0, #0x39             /* new TTBR active, MMU still on */
    bl rm443_boot_marker_phys /* private table maps the two MMIO pages */
.Lreloc_identity_branch:
    bx r11
.global arm926_relocate_identity
arm926_relocate_identity:
    mov r4, #0
    mrc p15, 0, r5, c1, c0, 0 /* do not retain SCTLR in marker-clobbered r5 */
    mov r6, #1
    mcr p15, 0, r6, c3, c0, 0
    bic r5, r5, #1
    mcr p15, 0, r5, c1, c0, 0
    nop
    nop
    nop
    mcr p15, 0, r4, c8, c7, 0
    /* Device MMIO is now physical. Stop independent writers before the
     * destination can overwrite firmware page tables, tasks and DSP buffers. */
    mov r0, #0x40
    bl rm443_boot_marker_phys
    bl rm443_quiesce
    str r0, [r8, #RELOC_QUIESCE_STATUS]
    str r3, [r8, #RELOC_DSP_BEFORE]
    str r1, [r8, #RELOC_DSP_AFTER]
    str r2, [r8, #RELOC_QUIESCE_DONE]
    ldr r9, [r8, #RELOC_TRACE]
    str r0, [r9, #TRACE_QUIESCE_STATUS]
    str r3, [r9, #TRACE_DSP_BEFORE]
    str r1, [r9, #TRACE_DSP_AFTER]
    str r2, [r9, #TRACE_QUIESCE_DONE]
    cmp r0, #0
    bne .Lreloc_failed
    /* Verify uncached physical source too: native's cached read alone cannot
     * exclude stale/modified backing RAM before ownership changed. */
    ldr r0, [r8, #RELOC_SOURCE]
    ldr r1, [r8, #RELOC_IMAGE_BYTES]
    bl arm926_image_hash
    str r0, [r9, #TRACE_SOURCE_HASH]
    ldr r1, [r8, #RELOC_EXPECTED_HASH]
    cmp r0, r1
    movne r0, #0x4c
    bne .Lreloc_integrity_failed
    mov r0, #0x41
    bl rm443_boot_marker_phys
    ldr r0, [r8, #RELOC_SOURCE]
    ldr r1, [r8, #RELOC_DESTINATION]
    ldr r2, [r8, #RELOC_IMAGE_BYTES]
    ldr r3, [r8, #RELOC_MEMORY_BYTES]
    bl arm926_relocate_payload
    ldr r0, [r8, #RELOC_IMAGE_BYTES]
    str r0, [r9, #TRACE_COPIED_BYTES]
    ldr r0, [r8, #RELOC_DESTINATION]
    ldr r1, [r8, #RELOC_IMAGE_BYTES]
    bl arm926_image_hash
    str r0, [r9, #TRACE_DEST_HASH]
    ldr r1, [r8, #RELOC_EXPECTED_HASH]
    cmp r0, r1
    movne r0, #0x4d
    bne .Lreloc_integrity_failed
    mov r0, #3                  /* copied Image + cleared BSS */
    str r0, [r9, #TRACE_RELOCATION_FLAGS]
    str r0, [r9, #TRACE_STAGE]
    mov r0, #0x42
    bl rm443_boot_marker_phys
    mrs r0, cpsr
    str r0, [r9, #TRACE_ENTRY_CPSR]
    mrc p15, 0, r0, c1, c0, 0
    str r0, [r9, #TRACE_ENTRY_SCTLR]
    mrc p15, 0, r0, c2, c0, 0
    str r0, [r9, #TRACE_ENTRY_TTBR]
    mrc p15, 0, r0, c3, c0, 0
    str r0, [r9, #TRACE_ENTRY_DACR]
    mrc p15, 0, r0, c13, c0, 0
    str r0, [r9, #TRACE_ENTRY_FCSE]
    mov r0, #0x43
    bl rm443_boot_marker_phys
    mov r0, #0
    mcr p15, 0, r0, c7, c10, 4  /* all physical writes visible before branch */
    mcr p15, 0, r0, c7, c5, 0
    ldr r3, [r8, #RELOC_DESTINATION]
    ldr r2, [r8, #RELOC_DTB]
    mvn r1, #0
    bx r3
.Lreloc_failed:
    mov r0, #0x4f
.Lreloc_integrity_failed:
    mov r1, #4
    str r1, [r9, #TRACE_STAGE]
    bl rm443_boot_marker_phys
    mov r0, #0
    mcr p15, 0, r0, c7, c10, 4
.Lreloc_park:
    b .Lreloc_park
.Lreloc_refuse:
    mvn r0, #0
    bx lr
.size arm926_relocate, .-arm926_relocate

/* FNV-1a over exact Image bytes, matching the build-time/native check.
 * r0=source, r1=bytes; returns hash in r0, clobbers r1-r3/ip only.
 * Runs with MMU/caches off; no stack or firmware services. */
.global arm926_image_hash
.type arm926_image_hash,%function
arm926_image_hash:
    ldr r2, =2166136261
    ldr r3, =16777619
    cmp r1, #0
    beq .Lhash_done
.Lhash_byte:
    ldrb ip, [r0], #1
    eor r2, r2, ip
    mul r2, r3, r2
    subs r1, r1, #1
    bne .Lhash_byte
.Lhash_done:
    mov r0, r2
    bx lr
.size arm926_image_hash, .-arm926_image_hash
.ltorg

/* Stackless physical memmove + BSS clear, exported for executable tests.
 * r0=source, r1=destination, r2=file bytes, r3=memory bytes (>=file).
 * Preserves r7-r11 and LR. Handles both overlap directions and odd lengths.
 */
.global arm926_relocate_payload
.type arm926_relocate_payload,%function
arm926_relocate_payload:
    mov r4, r1
    mov r5, r2
    mov r6, r3
    cmp r0, r1
    beq .Lzero_bss
    cmp r1, r0
    blo .Lcopy_forward
    add ip, r0, r2
    cmp r1, ip
    bhs .Lcopy_forward
    add r0, r0, r2
    add r1, r1, r2
.Lcopy_backward:
    cmp r2, #0
    beq .Lzero_bss
    ldrb ip, [r0, #-1]!
    strb ip, [r1, #-1]!
    subs r2, r2, #1
    bne .Lcopy_backward
    b .Lzero_bss
.Lcopy_forward:
    orr ip, r0, r1
    tst ip, #3
    bne .Lcopy_bytes
.Lcopy_words:
    cmp r2, #4
    blo .Lcopy_bytes
    ldr ip, [r0], #4
    str ip, [r1], #4
    sub r2, r2, #4
    b .Lcopy_words
.Lcopy_bytes:
    cmp r2, #0
    beq .Lzero_bss
    ldrb ip, [r0], #1
    strb ip, [r1], #1
    subs r2, r2, #1
    bne .Lcopy_bytes
.Lzero_bss:
    add r1, r4, r5
    sub r2, r6, r5
    mov ip, #0
.Lzero_bytes:
    cmp r2, #0
    bxeq lr
    strb ip, [r1], #1
    subs r2, r2, #1
    bne .Lzero_bytes
    bx lr
.size arm926_relocate_payload, .-arm926_relocate_payload

#include "rm443_quiesce.inc"
#include "rm443_marker.inc"
