/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>
#include <asm/errno.h>

.syntax unified
.arch armv5te
.arm
.text
.balign 4
/* r0=Linux-mapped address, r1=width (validated), r2=aligned u32 result.
 * Each successful access is exactly one instruction of the requested width.
 * Translation/permission faults use __ex_table. The board's precise external
 * abort hook accepts only these three PCs and uses the same fixup. */
ENTRY(rm443_probe_load)
	cmp r1, #1
	beq rm443_probe_load8
	cmp r1, #2
	beq rm443_probe_load16
	.global rm443_probe_load32
rm443_probe_load32:
	ldr r3, [r0]
	b .Lloaded
	.global rm443_probe_load16
rm443_probe_load16:
	ldrh r3, [r0]
	b .Lloaded
	.global rm443_probe_load8
rm443_probe_load8:
	ldrb r3, [r0]
.Lloaded:
	str r3, [r2]
	mov r0, #0
	bx lr
.Lfault:
	mvn r0, #(EFAULT - 1)
	bx lr
ENDPROC(rm443_probe_load)

.pushsection __ex_table,"a"
.balign 8
.long rm443_probe_load8, .Lfault
.long rm443_probe_load16, .Lfault
.long rm443_probe_load32, .Lfault
.popsection
