/* SPDX-License-Identifier: MIT */
/* Position-independent Thumb example; enter at buffer physical address | 1. */
.syntax unified
.arch armv5te
.thumb
.text
.balign 2
.global probe_entry
.thumb_func
.type probe_entry, %function
probe_entry:
    movs r2, r0
    adds r0, r0, r1
    eors r1, r2
    bx lr
.size probe_entry, .-probe_entry
