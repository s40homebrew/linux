# SPDX-License-Identifier: MIT
"""Host tests of actual probe staging/mapping C, plus ARM assembly checks.

No ARM code is executed here. The only substituted operation is entry into
the privileged bridge, whose CPU transition still needs handset validation.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from support import function

ROOT = Path(__file__).resolve().parents[1]
MACH = ROOT / "kernel/overlay/arch/arm/mach-nokia-raps"
UAPI = ROOT / "kernel/overlay/include/uapi"


class ProbeBridge(unittest.TestCase):
    def test_staging_and_private_mapping_translation(self):
        source = (MACH / "rm443_secure_wdt.c").read_text()
        # The shared extractor locates static declarations; linkage is the
        # only change needed to extract these exported board-local helpers.
        source = source.replace("\nint rm443_probe_", "\nstatic int rm443_probe_")
        body = "\n".join(function(source, name) for name in (
            "rm443_probe_bridge_info", "rm443_probe_translate", "rm443_probe_execute"))
        binding = "bridge = (void *)swd + RM443_SWD_CODE_OFFSET;"
        self.assertEqual(body.count(binding), 1)
        body = body.replace(binding, "bridge = simulated_return;")
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "rm443_secure_wdt_abi.h"
#include <linux/rm443_probe.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define SZ_1K 1024U
#define SZ_4K 4096U
#define SZ_1M 1048576U
#define RM443_PROBE_MAP_BASE 0x40000000U
#define RM443_PROBE_MAP_SLOT_BYTES RM443_PROBE_MAX_OBJECT
#define min(a,b) ((a) < (b) ? (a) : (b))
static _Alignas(16384) unsigned char retained[RM443_SWD_BYTES], saved[RM443_SWD_BYTES];
static struct rm443_swd_state *swd;
static unsigned swd_successes, rm443_rom_lock = 1;
static bool swd_stopped, preempt_off, irq_off;
static int power_error;
static unsigned feeds, entries, times;
static struct rm443_probe_call *expected;
static u32 *probe_active_mapping_table(u32 address, u32 descriptor, u32 *remaining) {
    (void)address; (void)descriptor; (void)remaining;
    return NULL; /* Dynamic table ownership is exercised in test_probe_mappings. */
}
static void lockdep_assert_held(unsigned *lock) { assert(*lock); }
static int rm443_power_refresh(void) {
    assert(!preempt_off && !irq_off); ++feeds; return power_error;
}
static void preempt_disable(void) { assert(!preempt_off); preempt_off = true; }
static void preempt_enable(void) { assert(preempt_off && !irq_off); preempt_off = false; }
#define local_irq_save(flags) do { flags = irq_off; irq_off = true; } while (0)
#define local_irq_restore(flags) do { irq_off = flags; } while (0)
static u64 ktime_get_ns(void) { assert(preempt_off && irq_off); return ++times * 100; }
static int simulated_return(void *state, u32 physical) {
    struct rm443_swd_probe_state *p = (void *)(retained + SWD_PROBE_MODE);
    struct rm443_probe_callback *records = (void *)(retained + SWD_PROBE_RECORDS);
    assert(state == swd && physical == swd->block_pa);
    assert(rm443_rom_lock && preempt_off && irq_off && p->mode == 1);
    assert(p->entry == expected->entry);
    for (unsigned i = 0; i < 13; ++i) {
        u32 want = i == 0 && (expected->flags & RM443_PROBE_CALL_ROM)
            ? swd->block_pa + SWD_PROBE_DESC : expected->regs[i];
        assert(p->regs[i] == want);
        assert(p->result[i] == 0);
    }
    for (unsigned i = 0; i < 16; ++i)
        assert(p->stack[i] == (i < expected->stack_words ? expected->stack[i] : 0));
    assert(!memcmp(p->descriptor, expected->descriptor, sizeof(p->descriptor)));
    assert(!p->count && !p->overflow && !p->cpsr);
    for (unsigned i = 0; i < sizeof(expected->callbacks); ++i)
        assert(!((unsigned char *)records)[i]);
    /* Staging must not overwrite even one byte of retained watchdog data,
     * its request/response, translation tables, code, stack or pool. */
    assert(!memcmp(retained, saved, SWD_PROBE_MODE));
    assert(!memcmp(retained + 0x800, saved + 0x800, sizeof(retained) - 0x800));
    for (unsigned i = 0; i < 13; ++i) p->result[i] = 0xface0000 + i;
    p->cpsr = 0xa00000df; p->count = 35; p->overflow = 3;
    for (unsigned i = 0; i < 32; ++i) {
        records[i].args[0] = i; records[i].args[1] = 4;
        records[i].result = 0x10200000 + i; records[i].stage = 12;
    }
    swd->stage = 30; swd->transport = 0xface0000;
    ++entries;
    return swd->transport;
}
'''
        main = r'''
int main(void) {
    struct rm443_probe_info info = {0};
    struct rm443_probe_call call = {0};
    u32 pa, span, *l1, *fine, *coarse;
    assert(rm443_probe_bridge_info(&info) == -ENODEV);
    assert(rm443_probe_translate(0, &pa, &span) == -ENODEV);
    assert(rm443_probe_execute(&call) == -ENODEV);
    memset(retained, 0xa5, sizeof(retained));
    swd = (void *)retained; swd_successes = 1;
    swd->block_pa = 0x11400000; swd->rom_entry = RM443_PROBE_ROM_ENTRY;
    swd->descriptor_pa = 0x01258c7c; swd->pa_header_pa = 0x02000000;
    swd->callback_va = 0xf03c4e1b; swd->work_pa = 0x11c06a00; swd->work_bytes = 8192;
    assert(!rm443_probe_bridge_info(&info));
    assert(info.bridge_pa == swd->block_pa && info.bridge_bytes == RM443_SWD_BYTES);
    assert(info.work_pa == swd->work_pa && info.work_bytes == swd->work_bytes);
    assert(info.rom_entry == swd->rom_entry && info.callback_va == swd->callback_va);
    assert(info.descriptor_pa == swd->descriptor_pa && info.pa_header_pa == swd->pa_header_pa);
    l1 = (void *)(retained + RM443_SWD_L1_OFFSET);
    fine = (void *)(retained + RM443_SWD_ROM_MAP_OFFSET);
    coarse = (void *)(retained + RM443_SWD_CALLBACK_L2);
    memset(l1, 0, 16384); memset(fine, 0, 4096); memset(coarse, 0, 1024);
    l1[0x114] = 0x11400c12; l1[0xc12] = 0x11400c12;
    assert(!rm443_probe_translate(0x11412345, &pa, &span));
    assert(pa == 0x11412345 && span == 0xedcbb);
    assert(!rm443_probe_translate(0xc12fffff, &pa, &span));
    assert(pa == 0x114fffff && span == 1);
    l1[0xfff] = (swd->block_pa + RM443_SWD_ROM_MAP_OFFSET) | 0x13;
    fine[0x3c2] = 0xffff0813;
    assert(!rm443_probe_translate(0xffff0809, &pa, &span));
    assert(pa == 0xffff0809 && span == 0x3f7);
    assert(rm443_probe_translate(0xffff0c00, &pa, &span) == -EFAULT);
    l1[0xf03] = (swd->block_pa + RM443_SWD_CALLBACK_L2) | 0x11;
    coarse[0xc4] = (swd->block_pa + RM443_SWD_CALLBACK_OFFSET) | 0xff2;
    assert(!rm443_probe_translate(0xf03c4e1b, &pa, &span));
    assert(pa == swd->block_pa + RM443_SWD_CALLBACK_OFFSET + 0xe1b && span == 0x1e5);
    assert(rm443_probe_translate(0xf03c5000, &pa, &span) == -EFAULT);
    l1[0x500] = 0xdead0013;
    assert(rm443_probe_translate(0x50000000, &pa, &span) == -EFAULT);
    l1[0x500] = (swd->block_pa + RM443_SWD_BYTES) | 0x11;
    assert(rm443_probe_translate(0x50000000, &pa, &span) == -EFAULT);
    fine[0x3c2] = 0xffff0802;
    assert(rm443_probe_translate(0xffff0809, &pa, &span) == -EFAULT);
    assert(rm443_probe_translate(0x70000000, &pa, &span) == -EFAULT);
    swd_stopped = true;
    assert(rm443_probe_execute(&call) == -ENODEV);
    assert(rm443_probe_translate(0x11400000, &pa, &span) == -ENODEV);
    swd_stopped = false;
    call.entry = 0x11500000;
    call.stack_words = 17; assert(rm443_probe_execute(&call) == -EINVAL);
    call.stack_words = 0; call.flags = 4; assert(rm443_probe_execute(&call) == -EINVAL);
    call.flags = RM443_PROBE_CALL_ROM | RM443_PROBE_CALL_VIRTUAL;
    assert(rm443_probe_execute(&call) == -EINVAL);
    call.flags = 0; call.reserved = 1; assert(rm443_probe_execute(&call) == -EINVAL);
    call.reserved = 0; call.entry = 0; assert(rm443_probe_execute(&call) == -EINVAL);
    call.entry = 0x11500002; assert(rm443_probe_execute(&call) == -EINVAL);
    assert(!feeds && !entries && !preempt_off && !irq_off);
    call.entry = 0x11500001; call.stack_words = 3;
    for (unsigned i = 0; i < 13; ++i) call.regs[i] = 0xfeed0000 + i;
    for (unsigned i = 0; i < 16; ++i) call.stack[i] = 0xfade0000 + i;
    for (unsigned i = 0; i < 4; ++i) call.descriptor[i] = 0x12345670 + i;
    memcpy(saved, retained, sizeof(saved));
    expected = &call;
    power_error = -EIO;
    assert(rm443_probe_execute(&call) == -EIO);
    assert(!((struct rm443_swd_probe_state *)(retained + SWD_PROBE_MODE))->mode);
    assert(feeds == 1 && !entries && !irq_off && !preempt_off);
    power_error = 0;
    for (unsigned rom = 0; rom < 2; ++rom) {
        if (rom) { call.entry = 0; call.flags = RM443_PROBE_CALL_ROM; }
        assert(!rm443_probe_execute(&call));
        if (rom) assert(call.entry == RM443_PROBE_ROM_ENTRY);
        assert(!irq_off && !preempt_off && call.elapsed_ns == 100);
        assert(call.stage == 30 && call.cpsr == 0xa00000df);
        assert(call.callback_count == 35 && call.callback_overflow == 3);
        assert(call.callbacks[31].args[0] == 31 && call.callbacks[31].result == 0x1020001f);
        assert(call.callbacks[31].stage == 12 && !call.callbacks[31].reserved[0]);
        for (unsigned i = 0; i < 13; ++i) assert(call.returned[i] == 0xface0000 + i);
        assert(!((struct rm443_swd_probe_state *)(retained + SWD_PROBE_MODE))->mode);
        assert(!memcmp(retained, saved, SWD_PROBE_MODE));
        assert(!memcmp(retained + 0x800, saved + 0x800, sizeof(retained) - 0x800));
    }
    assert(entries == 2 && feeds == 3);
    return 0;
}
'''
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="rm443-probe-bridge-") as temporary:
            path = Path(temporary)
            (path / "linux").mkdir()
            (path / "linux/types.h").write_text("#include <stdint.h>\ntypedef uint32_t __u32; typedef int32_t __s32;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n")
            (path / "linux/ioctl.h").write_text("")
            (path / "bridge.c").write_text(harness + body + main)
            subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(path), "-I", str(MACH), "-I", str(UAPI),
                            str(path / "bridge.c"), "-o", str(path / "bridge")], check=True)
            subprocess.run([str(path / "bridge")], check=True)


if __name__ == "__main__":
    unittest.main()
