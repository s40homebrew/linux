/* SPDX-License-Identifier: MIT */
/* Position-independent ARM public-context example: r0=a+b, r1=a^b. */
.syntax unified
.arch armv5te
.arm
.text
.balign 4
.global probe_entry
.type probe_entry, %function
probe_entry:
    mov r2, r0
    add r0, r0, r1
    eor r1, r2, r1
    bx lr
.size probe_entry, .-probe_entry
