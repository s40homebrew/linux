/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Physical-only RM-443 takeover, called with MMU/caches off and IRQ/FIQ masked.
 * No stack or firmware calls. Preserves r8-r11; clobbers r0-r7/ip.
 * r0: zero or bounded failure (0x101..106 linear, 0x201..204 circular,
 *     0x301 DSP control write, 0x401 USB request/IRQ disable).
 * r1: DSP control after; r2: completed bits DSP1/linear2/circular4/USB8;
 * r3: DSP control before.
 *
 * DMA zero-command polling and linear-enable clearing follow recovered
 * firmware F037BDF0. DSP bit0-clear is inferred from NOLO writing 0x40 before
 * DDR setup and MCUSW writing 1 to release the DSP. Other bits are preserved.
 * The poll checks register readback; DSP acknowledgement and RF/modem shutdown
 * remain unverified. The emulator stops modeled DSP writes on this operation.
 * Shared clocks and rails stay intact.
 */
.balign 4
.global rm443_quiesce
.type rm443_quiesce,%function
rm443_quiesce:
    mov r7, lr
    mov r6, #0
    ldr r0, =0x20040504
    mov r1, #0
    str r1, [r0]                 /* inherited ASIC watchdog keepalive */

    ldr r0, =0x20032d18
    ldr r5, [r0]
    bic r1, r5, #1
    str r1, [r0]
    ldr r4, =100000
.Lquiesce_dsp_poll:
    ldr r1, [r0]
    tst r1, #1
    beq .Lquiesce_dsp_done
    subs r4, r4, #1
    bne .Lquiesce_dsp_poll
    ldr r0, =0x301
    b .Lquiesce_return
.Lquiesce_dsp_done:
    orr r6, r6, #1

    ldr r0, =0x20040c08
    mov r2, #0
    mov r1, #0
.Lquiesce_linear:
    str r1, [r0]
    ldr r4, =100000
.Lquiesce_linear_poll:
    ldr r3, [r0]
    cmp r3, #0
    beq .Lquiesce_linear_next
    subs r4, r4, #1
    bne .Lquiesce_linear_poll
    add r0, r2, #0x100
    add r0, r0, #1
    b .Lquiesce_return
.Lquiesce_linear_next:
    str r1, [r0, #-4]          /* F037BDF0: disable only after command is idle */
    add r0, r0, #0x400
    add r2, r2, #1
    cmp r2, #6
    blo .Lquiesce_linear
    orr r6, r6, #2

    ldr r0, =0x20042440
    mov r2, #0
.Lquiesce_circular:
    str r1, [r0]
    ldr r4, =100000
.Lquiesce_circular_poll:
    ldr r3, [r0]
    cmp r3, #0
    beq .Lquiesce_circular_next
    subs r4, r4, #1
    bne .Lquiesce_circular_poll
    add r0, r2, #0x200
    add r0, r0, #1
    b .Lquiesce_return
.Lquiesce_circular_next:
    add r0, r0, #4
    add r2, r2, #1
    cmp r2, #4
    blo .Lquiesce_circular
    orr r6, r6, #4

    /* Match the recovered 16-bit UDC register accesses. Disable request
     * generation before Linux can reclaim the firmware's DMA buffers.
     * Do not turn off shared clock resources or touch Betty here. */
    ldr r0, =0x20036600
    strh r1, [r0, #0x28]        /* IRQ_EN */
    strh r1, [r0, #0x2c]        /* DMA_IRQ_EN */
    strh r1, [r0, #0x40]        /* RX_DMA_CFG */
    strh r1, [r0, #0x44]        /* TX_DMA_CFG */
    ldrh r3, [r0, #0x18]
    bic r3, r3, #1
    strh r3, [r0, #0x18]        /* SYSCON1 pullup off */
    ldrh r3, [r0, #0x78]
    bic r3, r3, #0x8000
    strh r3, [r0, #0x78]        /* firmware connect extension */
    ldr r4, =100000
.Lquiesce_usb_poll:
    ldrh r3, [r0, #0x28]
    ldrh ip, [r0, #0x2c]
    orr r3, r3, ip
    ldrh ip, [r0, #0x40]
    orr r3, r3, ip
    ldrh ip, [r0, #0x44]
    orrs r3, r3, ip
    beq .Lquiesce_usb_done
    subs r4, r4, #1
    bne .Lquiesce_usb_poll
    ldr r0, =0x401
    b .Lquiesce_return
.Lquiesce_usb_done:
    orr r6, r6, #8
    mov r0, #0
.Lquiesce_return:
    ldr r1, =0x20032d18
    ldr r1, [r1]
    mov r2, r6
    mov r3, r5
    ldr ip, =0x20040504
    mov r4, #0
    str r4, [ip]
    bx r7
.size rm443_quiesce, .-rm443_quiesce
.ltorg
