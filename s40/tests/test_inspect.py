# SPDX-License-Identifier: MIT
"""Exercise retained inspector validation and bounded reads from actual C helpers."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import function

DRIVER = Path(__file__).resolve().parents[1] / "kernel/overlay/arch/arm/mach-nokia-raps/rm443_inspect.c"


class Inspector(unittest.TestCase):
    def test_actual_range_trace_and_read_helpers(self):
        source = DRIVER.read_text()
        helpers = "\n".join(function(source, name) for name in
                            ("inspect_range", "inspect_trace", "inspect_read"))
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t loff_t;
struct file { int unused; };
struct kobject { int unused; };
struct bin_attribute { struct { const char *name; } attr; size_t size; void *private; };
#define PAGE_SIZE 4096
#define GFP_KERNEL 0
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define kmalloc(n, flags) malloc(n)
#define kfree(p) free(p)
#define local_irq_save(flags) do { (flags) = 0; } while (0)
#define local_irq_restore(flags) ((void)(flags))
static u8 trace_snapshot[128], ramlog[PAGE_SIZE], secure_data[65536];
static void *secure_state = secure_data;
static int rm443_ram_log_snapshot(void *buffer, size_t size) {
    if (size != PAGE_SIZE) return -EINVAL;
    memcpy(buffer, ramlog, size); return 0;
}
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (0)
'''
        main = r'''
int main(void) {
    CHECK(inspect_range(0x10200000, 0x1e00000));
    CHECK(inspect_range(0x11ffffff, 1));
    CHECK(!inspect_range(0x10000000, 4096));
    CHECK(!inspect_range(0x20000000, 4096));
    CHECK(!inspect_range(0xffff0000, 65536));
    CHECK(!inspect_range(0x10200000, UINT64_MAX));
    CHECK(!inspect_range(0x11ffffff, 2));
    CHECK(!inspect_range(0x10200000, 0));
    u32 trace[32] = {0};
    trace[0]=0x4c303453; trace[1]=0x3130584e; trace[2]=3;
    trace[4]=0x10200000; trace[5]=0x1e00000; trace[6]=0x1100f000;
    trace[15]=0x11000000; trace[16]=0x10000;
    CHECK(inspect_trace(trace, 0x11000000, 0x1100f000));
    for (unsigned i=0; i<7; i++) {
        unsigned word[] = {0,1,2,4,6,15,16};
        u32 saved=trace[word[i]]; trace[word[i]]=0;
        CHECK(!inspect_trace(trace, 0x11000000, 0x1100f000));
        trace[word[i]]=saved;
    }
    for (unsigned i=0; i<sizeof(trace_snapshot); i++) trace_snapshot[i]=(u8)i;
    for (unsigned i=0; i<PAGE_SIZE; i++) ramlog[i]=(u8)(i*3);
    memset(secure_data, 0xa5, sizeof(secure_data));
    struct bin_attribute a = { .attr.name="boot_trace", .size=128 };
    unsigned char buffer[256]; memset(buffer,0xcc,sizeof(buffer));
    CHECK(inspect_read(NULL,NULL,&a,(char*)buffer,-1,10)==-EINVAL);
    CHECK(inspect_read(NULL,NULL,&a,(char*)buffer,128,10)==0);
    CHECK(inspect_read(NULL,NULL,&a,(char*)buffer,123,100)==5);
    CHECK(!memcmp(buffer,trace_snapshot+123,5)); CHECK(buffer[5]==0xcc);
    a.attr.name="ramlog"; a.size=PAGE_SIZE;
    CHECK(inspect_read(NULL,NULL,&a,(char*)buffer,4090,100)==6);
    CHECK(!memcmp(buffer,ramlog+4090,6)); CHECK(buffer[6]==0xcc);
    a.attr.name="secure_state"; a.size=172;
    CHECK(inspect_read(NULL,NULL,&a,(char*)buffer,170,100)==2);
    CHECK(buffer[0]==0xa5 && buffer[1]==0xa5);
    return 0;
}
'''
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("C compiler needed")
        with tempfile.TemporaryDirectory(prefix="rm443-inspect-test-") as temporary:
            path = Path(temporary)
            c = path / "test.c"
            binary = path / "test"
            c.write_text(harness + helpers + main)
            subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", str(c), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
