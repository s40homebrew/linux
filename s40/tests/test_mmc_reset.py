# SPDX-License-Identifier: MIT
"""Check MMC reset against POW/clock prerequisites from firmware and SPRU765A.

Unlike QEMU's immediate reset-done bit, this model holds reset while CON.POW
or the divider is zero. It exercises the driver's actual reset helper.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import function

DRIVER = Path(__file__).resolve().parents[1] / "kernel/overlay/arch/arm/mach-nokia-raps/rm443_mmc.c"


class MmcReset(unittest.TestCase):
    def test_reset_releases_power_and_clock_before_waiting(self):
        source = DRIVER.read_text()
        defines = source[source.index("#define MMC_CMD"):source.index("struct rm443_mmc {")]
        helpers = "\n".join(function(source, name) for name in (
            "rm443_read", "rm443_write", "rm443_mmc_select_clock", "rm443_mmc_reset"))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) (((1U << ((h) + 1)) - 1) & ~((1U << (l)) - 1))
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (0)
struct rm443_mmc {
    void *mmc;
    unsigned char *io, *clock;
    unsigned long power_on_after;
};
static unsigned char io[128], clocks[512];
static unsigned long jiffies;
static unsigned reset_sources;
static bool reset_requested;
static bool clock_gate, source3_clock = true, completed;
static unsigned long last_off;
static u32 load(const void *address) { u32 v; memcpy(&v, address, 4); return v; }
static void store(void *address, u32 value) { memcpy(address, &value, 4); }
static u32 readl(const void *address) {
    if (address == io + 0x68) {
        /* A missing functional clock or partial reset prevents completion. */
        u32 con = load(io + 0x0c);
        u32 source = (load(clocks + 0x10) >> 10) & 3;
        if (reset_requested && clock_gate && (source != 3 || source3_clock) &&
            (con & BIT(11)) && (con & 1023))
            completed = true;
        return completed;
    }
    return load(address);
}
static void writel(u32 value, void *address) {
    if (address == io + 0x64 && (value & 2)) {
        assert(jiffies >= last_off + 200);
        reset_requested = true;
        reset_sources |= BIT((load(clocks + 0x10) >> 10) & 3);
        completed = false;
        store(io + 0x0c, 0);
    }
    if (address == io + 0x0c && !(value & BIT(11))) last_off = jiffies;
    store(address, value);
}
static void msleep(unsigned ms) { jiffies += ms; }
#define msecs_to_jiffies(ms) (ms)
#define mmc_dev(mmc) (mmc)
static void dev_err(void *dev, const char *format, ...) {
    (void)dev; (void)format;
}
static void dev_warn(void *dev, const char *format, ...) {
    (void)dev; (void)format;
}
static void dev_info(void *dev, const char *format, ...) {
    (void)dev; (void)format;
}
#define readl_poll_timeout(addr, value, condition, interval, timeout) ({ \
    int result = -ETIMEDOUT; \
    for (unsigned elapsed = 0; elapsed <= (timeout); elapsed += (interval)) { \
        (value) = readl(addr); \
        if (condition) { result = 0; break; } \
    } \
    result; \
})
'''
        main = r'''
int main(void) {
    struct rm443_mmc host = { .io = io, .clock = clocks };
    const u32 other_clocks = 0xa5a5f3ff & ~GENMASK(11, 10);
    store(clocks + 0x10, other_clocks | BIT(11));
    store(io + 0x0c, 0x8805); /* inherited powered-on four-bit slot */
    clock_gate = true;
    CHECK(rm443_mmc_reset(&host) == 0);
    CHECK(completed && reset_sources == BIT(3));
    CHECK(load(io + 0x0c) == 0 && load(io + 0x14) == 0);
    CHECK(host.power_on_after >= last_off + 200);
    CHECK(load(clocks + 0x10) == (other_clocks | BIT(10)));

    /* Reproduce the physical failure: source3 reads back, but reset stays
     * pending. Source1 must get a fresh reset and its own 200 ms off time. */
    source3_clock = false; reset_sources = 0;
    CHECK(rm443_mmc_reset(&host) == 0);
    CHECK(completed && reset_sources == (BIT(3) | BIT(1)));
    CHECK(host.power_on_after >= last_off + 200);
    CHECK(load(clocks + 0x10) == (other_clocks | BIT(10)));

    /* Neither clock available must still fail and leave the slot off. */
    clock_gate = false; reset_sources = 0;
    CHECK(rm443_mmc_reset(&host) == -ETIMEDOUT);
    CHECK(!completed && reset_sources == (BIT(3) | BIT(1)));
    CHECK(load(io + 0x0c) == 0);
    CHECK(host.power_on_after >= last_off + 200);
    CHECK(load(clocks + 0x10) == (other_clocks | BIT(10)));
    puts("MMC reset: normal source, source1 recovery, and both-clocks-failed paths passed");
    return 0;
}
'''
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="rm443-mmc-reset-") as temporary:
            path = Path(temporary)
            (path / "reset.c").write_text(harness + defines + helpers + main)
            subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            str(path / "reset.c"), "-o", str(path / "reset")], check=True)
            subprocess.run([str(path / "reset")], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
