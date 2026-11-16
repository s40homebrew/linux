# SPDX-License-Identifier: MIT
"""Host checks of real mapping ownership, rollback and private-table changes.

No ARM instructions, ROM service or handset command is executed.
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

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "rm443_secure_wdt_abi.h"
#include <linux/rm443_probe.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define PAGE_SIZE 4096U
#define SZ_1K 1024U
#define SZ_4K 4096U
#define SZ_1M 1048576U
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define min(a,b) ((a)<(b)?(a):(b))
#define GFP_KERNEL 1U
#define __GFP_NORETRY 2U
#define __GFP_NOMEMALLOC 4U
#define __GFP_NOWARN 8U
#define RM443_PROBE_MAP_BASE 0x40000000U
#define RM443_PROBE_MAP_SLOT_BYTES RM443_PROBE_MAX_OBJECT
typedef int (*rm443_probe_page_physical)(void *, u32, u32 *);
static _Alignas(16384) u8 retained[RM443_SWD_BYTES];
static struct rm443_swd_state *swd;
static unsigned int swd_successes, rm443_rom_lock = 1;
static bool swd_stopped;
static unsigned int allocation_event, fail_event, live_descriptors, live_pages;
static struct { void *address; u32 physical; } allocated[256];
static void lockdep_assert_held(unsigned *lock) { assert(*lock); }
static bool should_fail(unsigned int flags) {
    assert(flags == (GFP_KERNEL | __GFP_NORETRY | __GFP_NOMEMALLOC | __GFP_NOWARN));
    return ++allocation_event == fail_event;
}
static void *kzalloc(size_t bytes, unsigned flags) {
    if (should_fail(flags)) return NULL;
    void *result = calloc(1, bytes); assert(result); live_descriptors++;
    return result;
}
static void kfree(void *p) { assert(p && live_descriptors); free(p); live_descriptors--; }
static unsigned long get_zeroed_page(unsigned int flags) {
    if (should_fail(flags)) return 0;
    for (unsigned int i = 0; i < 256; i++) {
        if (allocated[i].address) continue;
        allocated[i].address = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
        assert(allocated[i].address);
        memset(allocated[i].address, 0, PAGE_SIZE);
        allocated[i].physical = 0x11d00000U + i * PAGE_SIZE;
        live_pages++;
        return (unsigned long)allocated[i].address;
    }
    assert(0); return 0;
}
static void free_page(unsigned long address) {
    for (unsigned int i = 0; i < 256; i++) {
        if ((unsigned long)allocated[i].address != address) continue;
        free(allocated[i].address); allocated[i].address = NULL; live_pages--;
        return;
    }
    assert(0);
}
static u32 virt_to_phys(const void *address) {
    uintptr_t p = (uintptr_t)address;
    for (unsigned int i = 0; i < 256; i++) {
        uintptr_t base = (uintptr_t)allocated[i].address;
        if (base && p >= base && p - base < PAGE_SIZE)
            return allocated[i].physical + p - base;
    }
    assert(0); return 0;
}
static int fail_page = -1, misaligned_page = -1;
static int backing_page(void *opaque, u32 index, u32 *physical) {
    assert(opaque == &retained);
    if (index == (u32)fail_page) return -EFAULT;
    *physical = 0x02000000U + index * PAGE_SIZE;
    if (index == (u32)misaligned_page) (*physical)++;
    return 0;
}
'''

MAIN = r'''
static void assert_empty(void) {
    assert(!live_pages && !live_descriptors && !probe_mappings_active);
    for (unsigned int i = 0; i < RM443_PROBE_MAX_BUFFERS; i++) assert(!probe_mappings[i]);
}
int main(void) {
    struct rm443_probe_mapping *first = NULL, *last = NULL;
    u32 physical, span, *l1 = (void *)(retained + RM443_SWD_L1_OFFSET);
    const u32 bytes = 4 * SZ_1M + 17;
    assert(!rm443_probe_mapping_cost(0));
    assert(!rm443_probe_mapping_cost(RM443_PROBE_MAX_OBJECT + 1));
    assert(rm443_probe_mapping_cost(1) == 2 * PAGE_SIZE);
    assert(rm443_probe_mapping_cost(bytes) == 3 * PAGE_SIZE);
    assert(rm443_probe_mapping_cost(RM443_PROBE_MAX_OBJECT) == 9 * PAGE_SIZE);
    assert(rm443_probe_mapping_attach_all() == -ENODEV);
    assert(rm443_probe_mapping_create(32, bytes, backing_page, &retained, &first) == -EINVAL);
    assert(rm443_probe_mapping_create(0, 0, backing_page, &retained, &first) == -EINVAL);
    assert(rm443_probe_mapping_create(0, bytes, NULL, &retained, &first) == -EINVAL);
    assert_empty();
    for (fail_event = 1; fail_event <= 3; fail_event++) {
        allocation_event = 0;
        assert(rm443_probe_mapping_create(0, bytes, backing_page, &retained, &first) == -ENOMEM);
        assert(!first); assert_empty();
    }
    fail_event = 0;
    fail_page = 257;
    assert(rm443_probe_mapping_create(0, bytes, backing_page, &retained, &first) == -EFAULT);
    assert_empty();
    fail_page = -1; misaligned_page = 1;
    assert(rm443_probe_mapping_create(0, bytes, backing_page, &retained, &first) == -EINVAL);
    assert_empty(); misaligned_page = -1;

    assert(!rm443_probe_mapping_create(0, bytes, backing_page, &retained, &first));
    assert(first->pages == 1025 && first->table_count == 5);
    assert(live_pages == 2 && live_descriptors == 1);
    assert(rm443_probe_mapping_address(first) == 0x40000000);
    struct rm443_probe_mapping *duplicate = NULL;
    assert(rm443_probe_mapping_create(0, 1, backing_page, &retained, &duplicate) == -EBUSY);
    assert(!duplicate);
    assert(!rm443_probe_mapping_create(31, 4097, backing_page, &retained, &last));
    assert(rm443_probe_mapping_address(last) == 0x7e000000);
    assert(!probe_mapping_table(first, 4)[1]); /* Partial final table is zeroed. */

    swd = (void *)retained; swd->block_pa = 0x11400000; swd_successes = 1;
    l1[0x114] = 0x11400c12;
    l1[0xfff] = (swd->block_pa + RM443_SWD_ROM_MAP_OFFSET) | 0x13;
    assert(rm443_probe_translate(0x40000000, &physical, &span) == -EFAULT);
    /* Collision in the last object must leave the first object untouched. */
    l1[0x7e0] = 0x12340012;
    assert(rm443_probe_mapping_attach_all() == -EBUSY);
    assert(!probe_mappings_active && !l1[0x400] && l1[0x7e0] == 0x12340012);
    l1[0x7e0] = 0;
    assert(!rm443_probe_mapping_attach_all());
    assert(rm443_probe_mapping_attach_all() == -EBUSY);
    assert(rm443_probe_mapping_update(first, 0, 0x11000000) == -EBUSY);
    assert(!rm443_probe_translate(0x40000003, &physical, &span));
    assert(physical == 0x02000003 && span == PAGE_SIZE - 3);
    assert(!rm443_probe_translate(0x40400010, &physical, &span));
    assert(physical == 0x02400010 && span == 1); /* Clamp logical tail. */
    assert(rm443_probe_translate(0x40400011, &physical, &span) == -EFAULT);
    assert(rm443_probe_translate(0x40500000, &physical, &span) == -EFAULT);
    assert(!rm443_probe_translate(0x7e001000, &physical, &span));
    assert(physical == 0x02001000 && span == 1);
    u32 descriptor = l1[0x400];
    l1[0x400] = 0x10000c12; /* Replacement sections never bypass ownership. */
    assert(rm443_probe_translate(0x40000000, &physical, &span) == -EFAULT);
    l1[0x400] = l1[0x7e0]; /* Nor may another object's table be substituted. */
    assert(rm443_probe_translate(0x40000000, &physical, &span) == -EFAULT);
    l1[0x400] = descriptor;
    rm443_probe_mapping_detach_all();
    assert(!probe_mappings_active);
    for (unsigned int i = 0x400; i < 0x800; i++) assert(!l1[i]);
    assert(l1[0x114] == 0x11400c12);
    assert(l1[0xfff] == ((swd->block_pa + RM443_SWD_ROM_MAP_OFFSET) | 0x13));
    assert(rm443_probe_translate(0x40000000, &physical, &span) == -EFAULT);

    /* COW changes only the selected physical page and requires no allocation. */
    unsigned int before = allocation_event;
    assert(!rm443_probe_mapping_update(first, 0, 0x11bff000));
    assert(allocation_event == before);
    assert(rm443_probe_mapping_update(first, first->pages, 0x11bff000) == -EINVAL);
    assert(rm443_probe_mapping_update(first, 0, 0x11bff001) == -EINVAL);
    assert(!rm443_probe_mapping_attach_all());
    assert(!rm443_probe_translate(0x40000005, &physical, &span));
    assert(physical == 0x11bff005);
    assert(!rm443_probe_translate(0x40001005, &physical, &span));
    assert(physical == 0x02001005);
    /* Cleanup can unwind an attached batch without a dangling table pointer. */
    rm443_probe_mapping_destroy(first);
    assert(!probe_mappings_active && !probe_mappings[0]);
    for (unsigned int i = 0x400; i < 0x800; i++) assert(!l1[i]);
    assert(!rm443_probe_mapping_attach_all());
    assert(rm443_probe_translate(0x40000000, &physical, &span) == -EFAULT);
    assert(!rm443_probe_translate(0x7e000000, &physical, &span));
    rm443_probe_mapping_destroy(last);
    assert_empty();
    return 0;
}
'''


class ProbeMappings(unittest.TestCase):
    def test_page_mapping_ownership_rollback_and_restore(self):
        source = (MACH / "rm443_secure_wdt.c").read_text()
        globals_start = source.index("#define PROBE_MAP_TABLE_PAGES")
        globals_end = source.index("static bool probe_mappings_active;") + len("static bool probe_mappings_active;")
        declarations = source[globals_start:globals_end]
        for result in ("int", "u32", "void"):
            source = source.replace(f"\n{result} rm443_probe_", f"\nstatic {result} rm443_probe_")
        bodies = "\n".join(function(source, name) for name in (
            "probe_mapping_table", "rm443_probe_mapping_cost", "rm443_probe_mapping_address",
            "rm443_probe_mapping_create", "rm443_probe_mapping_detach_all",
            "rm443_probe_mapping_destroy", "rm443_probe_mapping_update",
            "rm443_probe_mapping_attach_all", "probe_active_mapping_table", "rm443_probe_translate"))
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix="rm443-probe-mappings-") as directory:
            path = Path(directory)
            (path / "linux").mkdir()
            (path / "linux/types.h").write_text(
                "#include <stdint.h>\ntypedef uint32_t __u32; typedef int32_t __s32;\n"
                "#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n")
            (path / "linux/ioctl.h").write_text("")
            (path / "mappings.c").write_text(HARNESS + declarations + bodies + MAIN)
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                            "-I", str(path), "-I", str(MACH), "-I", str(UAPI),
                            str(path / "mappings.c"), "-o", str(path / "mappings")], check=True)
            subprocess.run([str(path / "mappings")], check=True)


if __name__ == "__main__":
    unittest.main()
