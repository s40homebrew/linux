/* SPDX-License-Identifier: MIT */
/* Hardware observation only. No MMIO, page table dereferences or CP15 writes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "s40/runtime.h"

struct cpu_state {
    uint32_t cpsr, midr, cache_type, control, ttbr, domains, fcse, pc, sp;
};

static void screen_line(unsigned row, const char *format, ...) {
    char text[33];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    unsigned count = strlen(text);
    if (row == 7 && count > 31)
        count = 31;
    memcpy((char *)(s40_result + 16) + 32 * row, text, count);
}

/* Keep externally visible in the ELF for disassembly/emulation checks. */
__attribute__((noinline)) int linux6303_read_cpu(struct cpu_state *s) {
    __asm__ volatile("mrs %0, cpsr" : "=r"(s->cpsr));
    /* Native execution need not imply privilege. Never try MRC from user mode. */
    if ((s->cpsr & 31) != 0x1f && (s->cpsr & 31) != 0x13)
        return -1;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(s->midr));
    if ((s->midr & 0xff0ffff0u) != 0x41069260u)
        return -2;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(s->cache_type));
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(s->control));
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(s->ttbr));
    __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(s->domains));
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 0" : "=r"(s->fcse));
    __asm__ volatile("mov %0, pc" : "=r"(s->pc));
    __asm__ volatile("mov %0, sp" : "=r"(s->sp));
    return 0;
}

int s40_app_main(void) {
    struct cpu_state s = {0};
    int status = linux6303_read_cpu(&s);
    if (status)
        s40_error("CPU probe refused (%d), CPSR %08x MIDR %08x", status, s.cpsr, s.midr);

    /* This tiny owned allocation is an address sample, NOT reserved Linux RAM.
     * The pointer stops being usable when this invocation returns. */
    void *sample = malloc(65536);
    FILE *out = fopen("D:\\linux6303-probe.json", "wb");
    int saved = 0;
    if (out) {
    fprintf(out, "{\n  \"format\": 1,\n  \"board\": \"nokia,rm443\",\n"
                 "  \"purpose\": \"register-snapshot-only\",\n");
#define FIELD(name, value) fprintf(out, "  \"%s\": \"0x%08x\",\n", name, (unsigned)(value))
    FIELD("cpsr", s.cpsr);
    FIELD("midr", s.midr);
    FIELD("cache_type", s.cache_type);
    FIELD("control", s.control);
    FIELD("ttbr", s.ttbr);
    FIELD("domains", s.domains);
    FIELD("fcse", s.fcse);
    FIELD("pc", s.pc);
    FIELD("sp", s.sp);
    FIELD("descriptor_va", (uintptr_t)s40_result);
    FIELD("sample_allocation_va", (uintptr_t)sample);
    FIELD("sample_allocation_bytes", sample ? 65536 : 0);
#undef FIELD
    fprintf(out, "  \"allocation_lifetime\": \"released-before-return-to-Java\",\n"
                 "  \"page_tables_captured\": false,\n  \"takeover_performed\": false\n}\n");
    int failed = ferror(out);
    if (fclose(out) != 0)
        failed = 1;
    saved = !failed;
    }
    /* The shared launcher displays eight fixed 32-character text chunks. */
    memset((char *)(s40_result + 16), ' ', 255);
    ((char *)(s40_result + 16))[255] = 0;
    screen_line(0, saved ? "Saved D:\\linux6303-probe.json" : "CPU captured; file not saved");
    screen_line(1, "CPSR %08X  MIDR %08X", s.cpsr, s.midr);
    screen_line(2, "CTRL %08X  TTBR %08X", s.control, s.ttbr);
    screen_line(3, "DACR %08X  FCSE %08X", s.domains, s.fcse);
    screen_line(4, "PC   %08X  SP   %08X", s.pc, s.sp);
    screen_line(5, "Cache type %08X", s.cache_type);
    screen_line(6, "Sample VA %08X (released)", (unsigned)(uintptr_t)sample);
    screen_line(7, "No MMIO / no Linux takeover");
    free(sample);
    return 0;
}
