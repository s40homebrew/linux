/* SPDX-License-Identifier: GPL-2.0-only */
/* Copied, position-independent ARM926 ROM call. No S40 OS code executes. */
#include "rm443_secure_wdt_abi.h"
.syntax unified
.arch armv5te
.arm
.text
.balign 32
.global rm443_swd_bridge_start
.global rm443_swd_bridge_end
.global rm443_swd_callback
.type rm443_swd_bridge_start, %function
/* r0 = Linux mapping of state; r1 = physical state/block base. */
rm443_swd_bridge_start:
.Lbridge_start:
    push {r4-r11, lr}
    mov r4, r0
    mov r8, r1
    str sp, [r4, #SWD_LINUX_SP]
    mrs r2, cpsr
    str r2, [r4, #SWD_LINUX_CPSR]
    orr r2, r2, #0xc0
    msr cpsr_c, r2
    mrc p15, 0, r2, c1, c0, 0
    str r2, [r4, #SWD_LINUX_CONTROL]
    mrc p15, 0, r3, c2, c0, 0
    str r3, [r4, #SWD_LINUX_TTBR]
    mrc p15, 0, r3, c3, c0, 0
    str r3, [r4, #SWD_LINUX_DOMAINS]
    mrc p15, 0, r3, c13, c0, 0
    str r3, [r4, #SWD_LINUX_FCSE]
    mrc p15, 0, r3, c13, c0, 1
    str r3, [r4, #SWD_LINUX_CONTEXT]
    adr r3, .Llinux_return
    str r3, [r4, #SWD_RETURN_VA]
    mov r3, #10
    str r3, [r4, #SWD_STAGE]
.Lclean_before:
    mrc p15, 0, apsr_nzcv, c7, c14, 3
    bne .Lclean_before
    mov r3, #0
    mcr p15, 0, r3, c7, c10, 4
    bic r2, r2, #0x1000
    bic r2, r2, #0x0e
    mcr p15, 0, r2, c1, c0, 0
    mcr p15, 0, r3, c7, c5, 0
    mcr p15, 0, r3, c13, c0, 0
    mcr p15, 0, r3, c13, c0, 1
    add r6, r8, #RM443_SWD_L1_OFFSET
    mov r5, #3
    mcr p15, 0, r5, c3, c0, 0
    mcr p15, 0, r6, c2, c0, 0
    mcr p15, 0, r3, c8, c7, 0
    adr r5, .Lphysical
    sub r5, r5, r4
    add r5, r5, r8
    bx r5
.Lphysical:
    ldr r2, [r8, #SWD_FW_CONTROL]
    bic r2, r2, #0x1000
    bic r2, r2, #0x0e
    orr r2, r2, #1
    mcr p15, 0, r2, c1, c0, 0
    nop
    nop
    nop
    /* The S40 security task enters ROM in privileged System mode. Borrow
     * that mode with a private stack, preserving Linux's shared user bank. */
    mrs r2, cpsr
    bic r2, r2, #31
    orr r2, r2, #0x1f
    msr cpsr_c, r2
    str sp, [r8, #SWD_SYSTEM_SP]
    str lr, [r8, #SWD_SYSTEM_LR]
    add sp, r8, #RM443_SWD_STACK_TOP
    ldr r0, [r8, #SWD_PROBE_MODE]
    cmp r0, #0
    bne .Lprobe_call
    sub sp, sp, #16
    add r0, r8, #RM443_SWD_REQUEST_OFFSET
    str r0, [sp]
    add r0, r8, #RM443_SWD_RESPONSE_OFFSET
    str r0, [sp, #4]
    mov r0, #2
    str r0, [sp, #8]
    ldr r0, [r8, #SWD_DESCRIPTOR]
    mov r1, #0x1f
    ldr r2, [r8, #SWD_PA_HEADER]
    mov r3, #0
    ldr r4, [r8, #SWD_ROM_ENTRY]
    blx r4
.Lcall_return:
    str r0, [r8, #SWD_TRANSPORT]
    mov r0, #20
    str r0, [r8, #SWD_STAGE]
    ldr sp, [r8, #SWD_SYSTEM_SP]
    ldr lr, [r8, #SWD_SYSTEM_LR]
    ldr r0, [r8, #SWD_LINUX_CPSR]
    orr r0, r0, #0xc0
    msr cpsr_c, r0
.Lclean_after:
    mrc p15, 0, apsr_nzcv, c7, c14, 3
    bne .Lclean_after
    mov r0, #0
    mcr p15, 0, r0, c7, c10, 4
    mrc p15, 0, r1, c1, c0, 0
    bic r1, r1, #0x1000
    bic r1, r1, #0x0e
    mcr p15, 0, r1, c1, c0, 0
    mcr p15, 0, r0, c7, c5, 0
    /* Load all saved values while physical RAM is still mapped. */
    ldr r1, [r8, #SWD_LINUX_CONTROL]
    ldr r2, [r8, #SWD_LINUX_TTBR]
    ldr r3, [r8, #SWD_LINUX_DOMAINS]
    ldr r4, [r8, #SWD_LINUX_SP]
    ldr r5, [r8, #SWD_LINUX_CPSR]
    ldr r6, [r8, #SWD_RETURN_VA]
    ldr r7, [r8, #SWD_TRANSPORT]
    ldr r10, [r8, #SWD_LINUX_FCSE]
    ldr r11, [r8, #SWD_LINUX_CONTEXT]
    mov r9, #30
    str r9, [r8, #SWD_STAGE]
    bx r6
.Llinux_return:
    /* This VA has the same physical backing in both translation tables. */
    mcr p15, 0, r10, c13, c0, 0
    mcr p15, 0, r11, c13, c0, 1
    mcr p15, 0, r2, c2, c0, 0
    mcr p15, 0, r0, c8, c7, 0
    mcr p15, 0, r3, c3, c0, 0
    mcr p15, 0, r1, c1, c0, 0
    nop
    nop
    nop
    mov sp, r4
    mov r0, r7
    /* Restore masks only after Linux translation and stack are restored. */
    msr cpsr_c, r5
    pop {r4-r11, pc}

.Lprobe_call:
    /* Sixteen AAPCS stack arguments plus an entry-address scratch word.
     * The caller's complete r0-r12 input is loaded only after staging these. */
    sub sp, sp, #80
    add r9, r8, #SWD_PROBE_STACK
    ldmia r9!, {r0-r7}
    stmia sp, {r0-r7}
    add r10, sp, #32
    ldmia r9, {r0-r7}
    stmia r10, {r0-r7}
    ldr r0, [r8, #SWD_PROBE_ENTRY]
    str r0, [sp, #64]
    mov r0, #12
    str r0, [r8, #SWD_STAGE]
    add lr, r8, #SWD_PROBE_REGS
    ldmia lr, {r0-r12}
    ldr lr, [sp, #64]
    blx lr
.Lprobe_return:
    /* Do not trust any callee-saved register from an uploaded routine.
     * Its contract is to return with System mode, SP and mappings intact. */
    push {r0-r12, lr}
    mrs r1, cpsr
    adr r8, .Lprobe_return
    sub r8, r8, #((.Lprobe_return - .Lbridge_start) & 255)
    sub r8, r8, #((.Lprobe_return - .Lbridge_start) & ~255)
    sub r8, r8, #RM443_SWD_CODE_OFFSET
    str r1, [r8, #SWD_PROBE_CPSR]
    add r2, r8, #SWD_PROBE_RESULT
    mov r3, sp
    mov r4, #13
.Lprobe_save_registers:
    ldr r0, [r3], #4
    str r0, [r2], #4
    subs r4, r4, #1
    bne .Lprobe_save_registers
    ldr r0, [sp]
    b .Lcall_return
.size rm443_swd_bridge_start, .-rm443_swd_bridge_start

.balign 4
/* ROM callback ABI recovered from S40: (1, bytes) -> physical allocation;
 * (2, physical allocation) releases it; (3, ignored) requests persistence.
 * Four independent slots match the firmware callback's four-slot contract.
 * r8 is NOT assumed to survive a ROM-internal callback invocation. */
rm443_swd_callback:
    push {r1-r7, lr}
    push {r0, r12}
.Lcallback_anchor:
    adr r4, .Lcallback_anchor
    sub r4, r4, #((.Lcallback_anchor - .Lbridge_start) & 255)
    sub r4, r4, #((.Lcallback_anchor - .Lbridge_start) & ~255)
    sub r4, r4, #RM443_SWD_CODE_OFFSET
    mov r7, #0
    ldr r2, [r4, #SWD_PROBE_MODE]
    cmp r2, #0
    beq .Lcallback_unrecorded
    ldr r2, [r4, #SWD_PROBE_COUNT]
    add r3, r2, #1
    str r3, [r4, #SWD_PROBE_COUNT]
    cmp r2, #SWD_PROBE_RECORD_MAX
    bhs .Lcallback_overflow
    add r7, r4, #SWD_PROBE_RECORDS
    add r7, r7, r2, lsl #5
    str r0, [r7]
    str r1, [r7, #4]
    ldr r2, [sp, #12]
    str r2, [r7, #8]
    ldr r2, [sp, #16]
    str r2, [r7, #12]
    ldr r2, [r4, #SWD_STAGE]
    str r2, [r7, #20]
    b .Lcallback_unrecorded
.Lcallback_overflow:
    ldr r2, [r4, #SWD_PROBE_OVERFLOW]
    add r2, r2, #1
    str r2, [r4, #SWD_PROBE_OVERFLOW]
.Lcallback_unrecorded:
    ldr r2, [r4, #SWD_CALLBACK_CALLS]
    add r2, r2, #1
    str r2, [r4, #SWD_CALLBACK_CALLS]
    cmp r0, #1
    beq .Lallocate
    cmp r0, #2
    beq .Lrelease
    cmp r0, #3
    bne .Lcallback_bad
    /* Keep the live secure work buffer in RAM. No flash write is invented. */
    ldr r2, [r4, #SWD_PERSIST_CALLS]
    add r2, r2, #1
    str r2, [r4, #SWD_PERSIST_CALLS]
    b .Lcallback_zero
.Lallocate:
    cmp r1, #0
    beq .Lcallback_bad
    cmp r1, #RM443_SWD_POOL_SLOT_BYTES
    bhi .Lcallback_bad
    mov r5, #0
.Lslot:
    add r6, r4, #SWD_FIRMWARE_ALLOC_PA
    ldr r2, [r6, r5, lsl #2]
    cmp r2, #0
    bne .Lslot_next
    add r6, r4, #SWD_ALLOCATIONS
    ldr r2, [r6, r5, lsl #2]
    cmp r2, #0
    beq .Lslot_found
.Lslot_next:
    add r5, r5, #1
    cmp r5, #RM443_SWD_POOL_SLOTS
    blo .Lslot
    b .Lcallback_bad
.Lslot_found:
    str r1, [r6, r5, lsl #2]
    add r0, r4, #RM443_SWD_POOL_OFFSET
    add r0, r0, r5, lsl #14
    b .Lcallback_return
.Lrelease:
    cmp r1, #0
    beq .Lcallback_zero
    /* A ROM allocation retained from S40 remains at its original physical
     * address until released. Its pages stay reserved for this Linux boot. */
    mov r5, #0
    add r6, r4, #SWD_FIRMWARE_ALLOC_PA
.Llegacy_slot:
    ldr r2, [r6, r5, lsl #2]
    cmp r1, r2
    beq .Llegacy_release
    add r5, r5, #1
    cmp r5, #RM443_SWD_POOL_SLOTS
    blo .Llegacy_slot
    add r2, r4, #RM443_SWD_POOL_OFFSET
    sub r2, r1, r2
    mov r3, r2, lsl #18
    cmp r3, #0
    bne .Lcallback_bad
    mov r2, r2, lsr #14
    cmp r2, #RM443_SWD_POOL_SLOTS
    bhs .Lcallback_bad
    add r3, r4, #SWD_ALLOCATIONS
    mov r0, #0
    str r0, [r3, r2, lsl #2]
    b .Lcallback_return
.Llegacy_release:
    mov r0, #0
    str r0, [r6, r5, lsl #2]
    add r6, r4, #SWD_FIRMWARE_ALLOC_VA
    str r0, [r6, r5, lsl #2]
    b .Lcallback_return
.Lcallback_bad:
    ldr r2, [r4, #SWD_CALLBACK_ERROR]
    add r2, r2, #1
    str r2, [r4, #SWD_CALLBACK_ERROR]
.Lcallback_zero:
    mov r0, #0
.Lcallback_return:
    cmp r7, #0
    strne r0, [r7, #16]
    add sp, sp, #8
    pop {r1-r7, pc}
.balign 4
rm443_swd_bridge_end:
