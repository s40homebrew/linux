/* SPDX-License-Identifier: GPL-2.0-or-later */
ENTRY(arm926_enter)
SECTIONS {
    . = 0;
    .text : { *(.text.entry) }
    /DISCARD/ : { *(.ARM.attributes) *(.comment) }
    ASSERT(SIZEOF(.text) <= 4096, "handoff must fit one page")
}
