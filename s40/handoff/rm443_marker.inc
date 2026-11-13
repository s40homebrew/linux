/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stackless, position-independent physical LCD checkpoint. r0=two hex digits;
 * preserves r8-r11 and SP, clobbers r0-r7/ip/flags. No firmware or RAM scratch.
 * A timeout abandons this diagnostic, never the boot. LCD +8 bit3 is transfer
 * DONE, not proven FIFO space: each pixel pair is a separate 4-byte transfer.
 * Uses the panel power and bus timing initialized by S40.
 */
.balign 4
/* Callable preflight entry, while the firmware stack is still valid.
 * r0=code, r1=verified uncached LCD VA. Preserve the ordinary C ABI. */
.global rm443_boot_marker_call
.type rm443_boot_marker_call,%function
rm443_boot_marker_call:
    push {r4-r11, ip, lr}
    bl rm443_boot_marker_at
    pop {r4-r11, ip, pc}
.size rm443_boot_marker_call, .-rm443_boot_marker_call

.global rm443_boot_marker_phys
.type rm443_boot_marker_phys,%function
rm443_boot_marker_phys:
    ldr r1, =0x20022200
.global rm443_boot_marker_at
rm443_boot_marker_at:
    mov r7, lr
    mov r6, r0
    /* Firmware parks the LCD bus pins after every GDI update (F0253778).
     * Reapply F0253928's active pin settings. Native preflight verifies that
     * its LCD and GPIO aliases have the same displacement as physical MMIO.
     * These are GPIO set/clear aliases, not whole-port replacements. */
    ldr ip, [r1, #8]
    bic ip, ip, #1
    str ip, [r1, #8]
    add r3, r1, #0xfa00       /* GPIO 0x20031c00 relative to LCD 0x20022200 */
    ldr ip, =0x0000ffff
    str ip, [r3, #0x28]
    ldr ip, =0x00005555
    str ip, [r3, #0x24]
    mov ip, #0x00ff0000
    str ip, [r3, #0xe8]
.macro marker_ready
    mov r0, #1024
.Lmarker_poll\@:
    ldr ip, [r1, #8]
    tst ip, #8
    bne .Lmarker_ready\@
    subs r0, r0, #1
    bne .Lmarker_poll\@
    mvn r0, #0
    bx r7
.Lmarker_ready\@:
.endm
.macro marker_transfer bytes, data
    ldr ip, [r1, #4]
    mov ip, ip, lsl #2
    mov ip, ip, lsr #22
    mov ip, ip, lsl #20         /* inherited panel/bus mode bits29:20 */
    mov r0, #2
    str r0, [r1, #8]
    mov r0, #1
    str r0, [r1, #8]
    orr ip, ip, #(\bytes - 1)
.if \data
    orr ip, ip, #0x40000
.endif
    str ip, [r1, #4]
.endm
.macro marker_command value
    marker_ready
    marker_transfer 1, 0
    mov r0, #\value
    str r0, [r1, #16]
.endm
.macro marker_data value
    marker_ready
    marker_transfer 4, 1
    ldr r0, =\value
    str r0, [r1, #16]
.endm
    marker_command 0x3a
    marker_ready
    marker_transfer 1, 1
    mov r0, #0x55
    str r0, [r1, #16]           /* DCS RGB565, counted one-byte transfer */
    marker_command 0x2a
    marker_data 0x3f000000      /* columns 0..63, MSB-first DCS bytes */
    marker_command 0x2b
    marker_data 0x27000000      /* rows 0..39 */
    marker_command 0x2c

    adr r3, .Lmarker_masks
    mov r0, r6, lsr #4
    and r0, r0, #15
    ldrb r2, [r3, r0]
    and r0, r6, #15
    ldrb r6, [r3, r0]
    orr r6, r6, r2, lsl #8
    mov r5, #0                 /* y */
.Lmarker_row:
    mov r4, #0                 /* x, pairs of identical pixels */
.Lmarker_pair:
    and r3, r4, #31            /* local x */
    mov r2, #0                 /* segment selector */
    cmp r3, #6
    blo .Lmarker_vertical
    cmp r3, #26
    bhs .Lmarker_vertical
    cmp r5, #2
    blo .Lmarker_pixel
    cmp r5, #6
    movlo r2, #1               /* A */
    blo .Lmarker_pixel
    cmp r5, #18
    blo .Lmarker_pixel
    cmp r5, #22
    movlo r2, #64              /* G */
    blo .Lmarker_pixel
    cmp r5, #34
    blo .Lmarker_pixel
    cmp r5, #38
    movlo r2, #8               /* D */
    b .Lmarker_pixel
.Lmarker_vertical:
    cmp r3, #2
    blo .Lmarker_pixel
    cmp r3, #30
    bhs .Lmarker_pixel
    cmp r5, #6
    blo .Lmarker_pixel
    cmp r5, #18
    bhs .Lmarker_lower
    cmp r3, #6
    movlo r2, #32              /* F */
    movhs r2, #2               /* B */
    b .Lmarker_pixel
.Lmarker_lower:
    cmp r5, #22
    blo .Lmarker_pixel
    cmp r5, #34
    bhs .Lmarker_pixel
    cmp r3, #6
    movlo r2, #16              /* E */
    movhs r2, #4               /* C */
.Lmarker_pixel:
    cmp r4, #32
    movlo r2, r2, lsl #8
    tst r6, r2
    mvnne r2, #0               /* two white RGB565 pixels */
    moveq r2, #0               /* two black pixels */
    marker_ready
    marker_transfer 4, 1
    str r2, [r1, #16]
    add r4, r4, #2
    cmp r4, #64
    blo .Lmarker_pair
    add r5, r5, #1
    cmp r5, #40
    blo .Lmarker_row
    marker_ready
    mov r0, #0                /* completed transfers, not panel readback */
    bx r7
.balign 4
.Lmarker_masks:
    .byte 0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07
    .byte 0x7f,0x6f,0x77,0x7c,0x39,0x5e,0x79,0x71
.ltorg
.size rm443_boot_marker_phys, .-rm443_boot_marker_phys
.purgem marker_ready
.purgem marker_transfer
.purgem marker_command
.purgem marker_data
