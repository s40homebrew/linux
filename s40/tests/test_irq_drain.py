# SPDX-License-Identifier: MIT
"""Exercise the actual RAPS IRQ drain with mapped, stuck and invalid sources."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from support import function

DRIVER = Path(__file__).resolve().parents[1] / "kernel/overlay/arch/arm/mach-nokia-raps/irq.c"


class IrqDrain(unittest.TestCase):
    def test_storm_budget_timer_interleave_and_bad_vectors(self):
        source = DRIVER.read_text()
        constants = "\n".join(re.findall(r"^#define RAPS_(?:IRQ_COUNT|TIMER_IRQ|IRQ_DRAIN_LIMIT|VECTOR|DISABLE)\s+\S+", source, re.M))
        code = function(source, "raps_irq_quarantine") + "\n" + function(source, "raps_handle_irq")
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
typedef uint16_t u16;
typedef uint32_t u32;
#define U32_MAX UINT32_MAX
#define __exception_irq_entry
struct pt_regs { unsigned unused; };
static unsigned char registers[64];
static void *raps_intc = registers;
static void *raps_domain;
static unsigned vectors[40000], count, position;
static unsigned handled[RAPS_IRQ_COUNT], disabled[RAPS_IRQ_COUNT], raw_masks[RAPS_IRQ_COUNT];
static unsigned quarantine_logs, warning_logs, logged_count;
static bool inside_handler, mapped[RAPS_IRQ_COUNT];
static u32 readl(void *address) {
    assert(address == registers + RAPS_VECTOR);
    assert(position <= count);
    return position < count ? vectors[position++] : U32_MAX;
}
static void writel(u32 value, void *address) {
    assert(address == registers + RAPS_DISABLE && value < RAPS_IRQ_COUNT);
    assert(!mapped[value] && !inside_handler);
    raw_masks[value]++;
}
static unsigned irq_find_mapping(void *domain, u32 source) {
    assert(domain == raps_domain && source < RAPS_IRQ_COUNT);
    return mapped[source] ? source + 1 : 0;
}
static void disable_irq_nosync(unsigned irq) {
    assert(irq && irq <= RAPS_IRQ_COUNT && !inside_handler);
    assert(mapped[irq - 1] && irq - 1 != RAPS_TIMER_IRQ);
    disabled[irq - 1]++;
}
static int generic_handle_domain_irq(void *domain, u32 source) {
    assert(domain == raps_domain && source < RAPS_IRQ_COUNT);
    assert(!disabled[source] && !raw_masks[source]);
    inside_handler = true;
    handled[source]++;
    inside_handler = false;
    return mapped[source] ? 0 : -1;
}
static void pr_err(const char *format, ...) {
    va_list args;
    (void)format;
    va_start(args, format);
    unsigned source = va_arg(args, unsigned);
    unsigned irq = va_arg(args, unsigned);
    logged_count = va_arg(args, unsigned);
    assert(source < RAPS_IRQ_COUNT);
    assert(irq == (mapped[source] ? source + 1 : 0));
    assert(disabled[source] + raw_masks[source] == 1);
    quarantine_logs++;
    va_end(args);
}
static void pr_err_ratelimited(const char *format, ...) { (void)format; warning_logs++; }
static void reset(void) {
    count = position = quarantine_logs = warning_logs = logged_count = 0;
    memset(handled, 0, sizeof(handled));
    memset(disabled, 0, sizeof(disabled));
    memset(raw_masks, 0, sizeof(raw_masks));
    for (unsigned i = 0; i < RAPS_IRQ_COUNT; i++) mapped[i] = true;
}
static void emit(unsigned source) {
    assert(count < sizeof(vectors) / sizeof(vectors[0]));
    vectors[count++] = source;
}
'''
        main = r'''
int main(void) {
    /* A busy but finite burst, including exactly the limit, stays enabled. */
    for (unsigned boot = 0; boot < 2; boot++) {
        reset();
        for (unsigned i = 0; i < RAPS_IRQ_DRAIN_LIMIT; i++) emit(153);
        raps_handle_irq(NULL);
        assert(handled[153] == RAPS_IRQ_DRAIN_LIMIT && !quarantine_logs);
    }
    /* Timer and a different peripheral do not reset the offender's budget. */
    reset();
    for (unsigned i = 0; i < RAPS_IRQ_DRAIN_LIMIT; i++) {
        emit(153); emit(RAPS_TIMER_IRQ); emit(154);
    }
    emit(153); emit(RAPS_TIMER_IRQ); emit(154); emit(139);
    raps_handle_irq(NULL);
    assert(disabled[153] == 1 && disabled[154] == 1 && quarantine_logs == 2);
    assert(handled[153] == RAPS_IRQ_DRAIN_LIMIT && handled[154] == RAPS_IRQ_DRAIN_LIMIT);
    assert(handled[RAPS_TIMER_IRQ] == RAPS_IRQ_DRAIN_LIMIT + 1 && handled[139] == 1);
    assert(logged_count == RAPS_IRQ_DRAIN_LIMIT && !warning_logs && position == count);
    /* Timer alone remains exempt even beyond the peripheral threshold. */
    reset();
    for (unsigned i = 0; i < 2 * RAPS_IRQ_DRAIN_LIMIT; i++) emit(RAPS_TIMER_IRQ);
    raps_handle_irq(NULL);
    assert(handled[RAPS_TIMER_IRQ] == 2 * RAPS_IRQ_DRAIN_LIMIT && !quarantine_logs);
    /* A quarantined source that ignores masking cannot pin this entry. */
    reset();
    for (unsigned i = 0; i < RAPS_IRQ_DRAIN_LIMIT + 3; i++) emit(153);
    raps_handle_irq(NULL);
    assert(handled[153] == RAPS_IRQ_DRAIN_LIMIT && disabled[153] == 1);
    assert(quarantine_logs == 1 && warning_logs == 1 && position == RAPS_IRQ_DRAIN_LIMIT + 2);
    /* Unmapped sources use the hardware fallback, and other IRQs still run. */
    reset(); mapped[225] = false;
    emit(225); emit(139);
    raps_handle_irq(NULL);
    assert(handled[225] == 1 && raw_masks[225] == 1 && handled[139] == 1);
    assert(quarantine_logs == 1 && logged_count == 1);
    reset(); mapped[225] = false;
    emit(225); emit(225); emit(225);
    raps_handle_irq(NULL);
    assert(handled[225] == 1 && raw_masks[225] == 1 && position == 2 && warning_logs == 1);
    /* Malformed vectors cannot index counters or prolong the drain. */
    reset(); emit(RAPS_IRQ_COUNT); emit(139);
    raps_handle_irq(NULL);
    assert(position == 1 && warning_logs == 1 && !quarantine_logs && !handled[139]);
    reset(); emit(U32_MAX); emit(139);
    raps_handle_irq(NULL);
    assert(position == 1 && !warning_logs && !handled[139]);
    return 0;
}
'''
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="rm443-irq-drain-") as temporary:
            path = Path(temporary)
            (path / "irq.c").write_text(constants + "\n" + harness + code + main)
            subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", str(path / "irq.c"), "-o", str(path / "irq")], check=True)
            subprocess.run([str(path / "irq")], check=True)


if __name__ == "__main__":
    unittest.main()
