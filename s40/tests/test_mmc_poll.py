# SPDX-License-Identifier: MIT
"""Exercise the actual status wait without enabling the MMC interrupt."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from support import MACH, function

DRIVER = MACH / "rm443_mmc.c"


class MmcPoll(unittest.TestCase):
    def test_poll_readiness_timeout_and_removal(self):
        source = DRIVER.read_text()
        wait = function(source, "rm443_wait")
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#ifndef ENOMEDIUM
#define ENOMEDIUM 123
#endif
typedef uint16_t u16;
#define MMC_STAT 0x10
#define MMC_IE 0x14
#define min_t(t,a,b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define msecs_to_jiffies(ms) (((ms) + 9) / 10)
struct rm443_mmc { void *mmc; unsigned event; };
static bool poll, present = true;
static unsigned usecs, ready_at, sleeps, delays, irq_waits, irq_enables;
static unsigned long jiffies;
static u16 ie;
static void advance(unsigned us) { usecs += us; jiffies = usecs / 10000; }
static bool rm443_mmc_get_cd(void *mmc) { (void)mmc; return present; }
static u16 rm443_read(struct rm443_mmc *host, unsigned reg) {
    (void)host; assert(reg == MMC_STAT);
    return ready_at && usecs >= ready_at ? 0x400 : 0;
}
static void rm443_write(struct rm443_mmc *host, unsigned reg, u16 value) {
    (void)host; assert(reg == MMC_IE); ie = value;
    if (value) { assert(!poll); ++irq_enables; }
}
static void udelay(unsigned us) { assert(us == 2); ++delays; advance(us); }
static void usleep_range(unsigned min, unsigned max) {
    assert(min == 20 && max == 40); ++sleeps;
    /* Reproduce the actual HZ=100, !HIGH_RES_TIMERS configuration. */
    advance(10000);
}
static void reinit_completion(unsigned *event) { *event = 0; }
static long wait_for_completion_timeout(unsigned *event, long ticks) {
    (void)event; assert(!poll); ++irq_waits; advance(ticks * 10000); return 0;
}
'''
        main = r'''
int main(void) {
    struct rm443_mmc host = {0};
    poll = true; ie = 0xffff; ready_at = 40;
    assert(rm443_wait(&host, 0x400, 100) == 0x400);
    assert(usecs == 40 && !ie && !sleeps && !irq_enables && !irq_waits);
    /* 32 KiB, one halfword every40us: no tick-sized sleep per word. */
    usecs = jiffies = 0;
    for (unsigned word = 0; word < 16384; ++word) {
        ready_at = usecs + 40;
        assert(rm443_wait(&host, 0x400, 166) == 0x400);
    }
    assert(usecs == 655360 && !sleeps && !irq_enables);
    ready_at = 0;
    unsigned before = usecs;
    assert(rm443_wait(&host, 0x400, jiffies + 2) == -ETIMEDOUT);
    assert(sleeps && usecs - before < 30000 && !ie);
    present = false;
    before = usecs;
    assert(rm443_wait(&host, 0x400, jiffies + 2) == -ENOMEDIUM);
    assert(usecs == before && !ie);
    present = true; poll = false;
    ready_at = usecs + 1;
    assert(rm443_wait(&host, 0x400, jiffies + 20) == 0x400);
    assert(irq_enables == 1 && irq_waits == 1 && !ie);
    return 0;
}
'''
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="rm443-mmc-poll-") as temporary:
            path = Path(temporary)
            (path / "poll.c").write_text(harness + wait + "\n" + main)
            subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            str(path / "poll.c"), "-o", str(path / "poll")], check=True)
            subprocess.run([str(path / "poll")], check=True)


if __name__ == "__main__":
    unittest.main()
