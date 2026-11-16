# SPDX-License-Identifier: MIT
"""Exercise actual probe ownership, paging/COW, budgets and fault decisions on host.

No ARM instruction, ROM entry or handset command is executed.
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from support import function

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "kernel/overlay/arch/arm/mach-nokia-raps/rm443_probe.c"
ABI = ROOT / "kernel/overlay/include/uapi/linux/rm443_probe.h"

HARNESS = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint32_t __u32;
typedef int32_t __s32;
#define __aligned_u64 uint64_t __attribute__((aligned(8)))
#define __user
#define GFP_KERNEL 0
#define __GFP_ZERO 0
#define __GFP_NORETRY 0
#define __GFP_NOWARN 0
#define __GFP_NOMEMALLOC 0
#define PROBE_GFP 0
#define _IOW(t,n,s) (n)
#define _IOR(t,n,s) (n)
#define _IOWR(t,n,s) (n)
#define PAGE_SIZE 4096U
#define PAGE_ALIGN(n) (((n) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define U32_MAX UINT32_MAX
#define min(a,b) ((a)<(b)?(a):(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define offset_in_page(a) ((a)&(PAGE_SIZE-1))
struct mutex { unsigned unused; };
static int fail_copy_in, fail_copy_out, fail_allocate, alive, calls, fail_at;
static unsigned held, rm443_rom_lock, maps_alive;
static long available_pages = 8192;
static void mutex_lock(unsigned *lock) { (void)lock; assert(!held); held=1; }
static void mutex_unlock(unsigned *lock) { (void)lock; assert(held); held=0; }
static long si_mem_available(void) { return available_pages; }
static void cond_resched(void) { }
static int copy_from_user(void *out, const void *in, size_t bytes) {
    if (fail_copy_in) return 1;
    memcpy(out,in,bytes); return 0;
}
static int copy_to_user(void *out, const void *in, size_t bytes) {
    if (fail_copy_out) return 1;
    memcpy(out,in,bytes); return 0;
}
static void *alloc_pages_exact(size_t bytes, int flags) {
    (void)flags; ++calls;
    if (fail_allocate || (fail_at && calls==fail_at)) return NULL;
    void *p=NULL; assert(!posix_memalign(&p,PAGE_SIZE,bytes));
    memset(p,0,bytes); alive++; available_pages-=bytes/PAGE_SIZE;
    return p;
}
static void free_pages_exact(void *p,size_t bytes) {
    assert(p && bytes && !(bytes%PAGE_SIZE)); free(p); alive--;
    available_pages+=bytes/PAGE_SIZE;
}
static unsigned long __get_free_page(int flags) { return (unsigned long)alloc_pages_exact(PAGE_SIZE,flags); }
static void free_page(unsigned long p) { free_pages_exact((void *)p,PAGE_SIZE); }
static u32 virt_to_phys(const void *p) { return (u32)(uintptr_t)p; }
static void flush_icache_range(unsigned long a,unsigned long b) { assert(b>a); }
struct rm443_probe_mapping { u32 count,slot,pa[8192]; };
static u32 rm443_probe_mapping_cost(u32 bytes) { return PAGE_SIZE + DIV_ROUND_UP(bytes,1048576U)*PAGE_SIZE; }
static int rm443_probe_mapping_create(unsigned slot,u32 bytes,
    int (*get)(void *,u32,u32 *),void *opaque,struct rm443_probe_mapping **out) {
    assert(held); struct rm443_probe_mapping *m=calloc(1,sizeof(*m)); assert(m);
    m->count=DIV_ROUND_UP(bytes,PAGE_SIZE); m->slot=slot;
    for(u32 i=0;i<m->count;i++) assert(!get(opaque,i,&m->pa[i]));
    maps_alive++; *out=m; return 0;
}
static void rm443_probe_mapping_destroy(struct rm443_probe_mapping *m) { assert(held); free(m); maps_alive--; }
static u32 rm443_probe_mapping_address(const struct rm443_probe_mapping *m) { return 0x40000000U+m->slot*0x02000000U; }
static int mapping_updates, fail_update;
static int rm443_probe_mapping_update(struct rm443_probe_mapping *m,u32 i,u32 pa) {
    assert(held && i<m->count); ++mapping_updates;
    if(fail_update && mapping_updates==fail_update) return -EINVAL;
    m->pa[i]=pa; return 0;
}
static int flash_reads, fail_flash_read;
static u8 flash_byte(u32 pa) { return (u8)(pa^(pa>>8)); }
static int probe_flash_read(u32 pa,void *out,u32 bytes,u32 *done) {
    assert(held); ++flash_reads; *done=0;
    if(fail_flash_read && flash_reads==fail_flash_read) return -EFAULT;
    for(u32 i=0;i<bytes;i++) ((u8 *)out)[i]=flash_byte(pa+i);
    *done=bytes; return 0;
}
struct pt_regs { unsigned long pc; bool user; };
static unsigned long instruction_pointer(struct pt_regs *r) { return r->pc; }
static bool user_mode(struct pt_regs *r) { return r->user; }
static unsigned fixups;
static bool can_fixup=true;
static int fixup_exception(struct pt_regs *r) { (void)r; fixups++; return can_fixup; }
const u8 rm443_probe_load8[1]={1},rm443_probe_load16[1]={2},rm443_probe_load32[1]={3};
'''

MAIN = r'''
static void empty_session(struct probe_session *s) {
    mutex_lock(&rm443_rom_lock);
    for(unsigned i=0;i<RM443_PROBE_MAX_BUFFERS;i++)
        if(s->buffers[i].id) probe_drop_buffer(s,&s->buffers[i]);
    mutex_unlock(&rm443_rom_lock);
    assert(!alive && !maps_alive && !s->charged);
}
int main(void) {
    struct probe_session s={.next_id=1,.budget=RM443_PROBE_MAX_TOTAL,.reserve=RM443_PROBE_MIN_HEADROOM};
    struct rm443_probe_buffer r={.bytes=1};
    struct probe_buffer *b;
    assert(!probe_alloc(&s,&r));
    assert(r.id==1 && s.charged==PAGE_SIZE && alive==1);
    b=probe_find(&s,r.id); assert(b && b->bytes==1);
    for(unsigned i=0;i<PAGE_SIZE;i++) assert(!((u8 *)b->data)[i]);
    assert(probe_buffer_range(b,1,0)); assert(!probe_buffer_range(b,2,0));
    assert(!probe_buffer_range(b,UINT32_MAX,2)); assert(!probe_buffer_range(b,0,UINT32_MAX));
    assert(!probe_owned_entry(&s,r.physical,false));
    empty_session(&s); assert(!probe_find(&s,r.id) && !probe_find(&s,0));
    r.bytes=16; assert(!probe_alloc(&s,&r));
    assert(probe_owned_entry(&s,r.physical,false));
    assert(probe_owned_entry(&s,(r.physical+14)|1,false));
    assert(!probe_owned_entry(&s,r.physical+16,false));
    assert(!probe_owned_entry(&s,r.physical,true));
    empty_session(&s);
    fail_copy_out=1; assert(probe_alloc(&s,&r)==-EFAULT && !s.charged && !alive); fail_copy_out=0;
    fail_allocate=1; assert(probe_alloc(&s,&r)==-ENOMEM); fail_allocate=0;
    fail_copy_in=1; assert(probe_alloc(&s,&r)==-EFAULT); fail_copy_in=0;
    r.bytes=0; assert(probe_alloc(&s,&r)==-EINVAL);
    r.bytes=RM443_PROBE_MAX_BUFFER+1; assert(probe_alloc(&s,&r)==-EINVAL);
    r.bytes=1; r.flags=1; assert(probe_alloc(&s,&r)==-EINVAL); r.flags=0;
    for(unsigned i=0;i<32;i++) assert(!probe_alloc(&s,&r));
    assert(probe_alloc(&s,&r)==-ENOSPC); empty_session(&s);
    r.bytes=RM443_PROBE_MAX_BUFFER;
    for(unsigned i=0;i<4;i++) assert(!probe_alloc(&s,&r));
    assert(s.charged==s.budget); r.bytes=1; assert(probe_alloc(&s,&r)==-ENOSPC); empty_session(&s);
    s.next_id=UINT32_MAX; assert(!probe_alloc(&s,&r) && r.id==UINT32_MAX);
    assert(probe_alloc(&s,&r)==-EOVERFLOW); empty_session(&s); s.next_id=100;

    /* Paged allocations use individual zeroed pages plus charged metadata. */
    assert(!probe_new_buffer(&s,RM443_PROBE_OBJECT_PAGED,2*PAGE_SIZE+7,0,&b));
    assert(!b->data && b->pages && b->page_count==3 && b->charged==4*PAGE_SIZE);
    for(unsigned i=0;i<3;i++) for(unsigned j=0;j<PAGE_SIZE;j++) assert(!((u8 *)b->pages[i])[j]);
    struct rm443_probe_object o;
    probe_object_description(b,&o); assert(!o.physical && o.kind==RM443_PROBE_OBJECT_PAGED);
    assert(!probe_owned_entry(&s,virt_to_phys(b->pages[0]),false));
    empty_session(&s);
    int before_alive=alive;
    fail_at=calls+3; /* metadata, first page, then second page allocation fails */
    assert(probe_new_buffer(&s,RM443_PROBE_OBJECT_PAGED,3*PAGE_SIZE,0,&b)==-ENOMEM);
    fail_at=0; assert(alive==before_alive && !s.charged);
    long saved_available=available_pages;
    available_pages=RM443_PROBE_MIN_HEADROOM/PAGE_SIZE;
    assert(probe_new_buffer(&s,RM443_PROBE_OBJECT_PAGED,PAGE_SIZE,0,&b)==-ENOMEM);
    available_pages=saved_available; assert(!s.charged && !alive);
    assert(!probe_object_valid(RM443_PROBE_OBJECT_FLASH,PAGE_SIZE,1));
    assert(!probe_object_valid(RM443_PROBE_OBJECT_FLASH,2*PAGE_SIZE,0x03fff000));
    assert(!probe_object_valid(RM443_PROBE_OBJECT_PAGED,RM443_PROBE_MAX_OBJECT+1,0));

    /* A full-size flash view initially costs only its page-pointer table. */
    assert(!probe_new_buffer(&s,RM443_PROBE_OBJECT_FLASH,RM443_PROBE_MAX_OBJECT,0x01000000,&b));
    assert(b->charged==PAGE_ALIGN(b->page_count*sizeof(void *)) && alive==1);
    probe_object_description(b,&o); assert(!o.physical && o.backing_physical==0x01000000);
    assert(probe_page_physical(b,5)==0x01005000);
    empty_session(&s);

    assert(!probe_new_buffer(&s,RM443_PROBE_OBJECT_FLASH,3*PAGE_SIZE,0x100000,&b));
    unsigned metadata=b->charged;
    u8 input[16],output[16]; memset(input,0xee,sizeof(input));
    mutex_lock(&rm443_rom_lock);
    assert(!probe_copy_out(b,PAGE_SIZE-4,output,sizeof(output)));
    for(unsigned i=0;i<sizeof(output);i++) assert(output[i]==flash_byte(0x100000+PAGE_SIZE-4+i));
    fail_at=calls+2; /* first COW page succeeds; second allocation fails */
    assert(probe_copy_in(&s,b,PAGE_SIZE-4,input,sizeof(input))==-ENOMEM);
    fail_at=0; assert(b->charged==metadata && !b->pages[0] && !b->pages[1]);
    fail_flash_read=flash_reads+2;
    assert(probe_copy_in(&s,b,PAGE_SIZE-4,input,sizeof(input))==-EFAULT);
    fail_flash_read=0; assert(b->charged==metadata && !b->pages[0] && !b->pages[1]);
    mutex_unlock(&rm443_rom_lock);
    o=(struct rm443_probe_object){.id=b->id};
    assert(!probe_object(&s,&o,RM443_PROBE_MAP));
    assert(o.bridge_va && o.mapped_bytes==3*PAGE_SIZE && !o.physical);
    unsigned mapped_charge=b->charged;
    assert(probe_owned_entry(&s,o.bridge_va,true));
    assert(!probe_owned_entry(&s,o.bridge_va,false));
    assert(!probe_owned_entry(&s,o.bridge_va+b->bytes,true));
    mutex_lock(&rm443_rom_lock);
    fail_update=mapping_updates+2;
    assert(probe_copy_in(&s,b,PAGE_SIZE-4,input,sizeof(input))==-EINVAL);
    fail_update=0;
    assert(b->charged==mapped_charge && !b->pages[0] && !b->pages[1]);
    assert(b->mapping->pa[0]==0x100000 && b->mapping->pa[1]==0x101000);
    assert(!probe_copy_in(&s,b,PAGE_SIZE-4,input,sizeof(input)));
    assert(b->charged==mapped_charge+2*PAGE_SIZE && b->pages[0] && b->pages[1] && !b->pages[2]);
    assert(b->mapping->pa[0]==virt_to_phys(b->pages[0]));
    assert(!probe_copy_out(b,PAGE_SIZE-4,output,sizeof(output)) && !memcmp(input,output,sizeof(input)));
    assert(((u8 *)b->pages[0])[0]==flash_byte(0x100000));
    assert(((u8 *)b->pages[1])[20]==flash_byte(0x101000+20));
    mutex_unlock(&rm443_rom_lock);
    o=(struct rm443_probe_object){.id=b->id};
    assert(!probe_object(&s,&o,RM443_PROBE_UNMAP) && !o.bridge_va && !maps_alive);
    assert(b->charged==metadata+2*PAGE_SIZE);
    fail_copy_out=1; o=(struct rm443_probe_object){.id=b->id};
    assert(probe_object(&s,&o,RM443_PROBE_MAP)==-EFAULT && !b->mapping && !maps_alive);
    fail_copy_out=0; empty_session(&s);
    o=(struct rm443_probe_object){.kind=RM443_PROBE_OBJECT_FLASH,.bytes=PAGE_SIZE,.backing_physical=0x100000};
    fail_copy_out=1; assert(probe_object(&s,&o,RM443_PROBE_OBJECT_CREATE)==-EFAULT);
    fail_copy_out=0; assert(!alive && !s.charged);

    struct rm443_probe_budget budget={.flags=RM443_PROBE_BUDGET_SET,.limit_bytes=8*1024*1024,.headroom_bytes=RM443_PROBE_MIN_HEADROOM};
    assert(!probe_budget(&s,&budget) && s.budget==8*1024*1024);
    budget.headroom_bytes=RM443_PROBE_MIN_HEADROOM-1; assert(probe_budget(&s,&budget)==-EINVAL);
    struct rm443_probe_read read={.address=0xfffffffc,.width=4,.bytes=4};
    assert(probe_read_valid(&read)); read.bytes=8; assert(!probe_read_valid(&read));
    read.address=0x1001;read.bytes=4; assert(!probe_read_valid(&read));
    read.width=1;assert(probe_read_valid(&read)); read.width=3;assert(!probe_read_valid(&read));
    read.width=2;read.address=0x1000;read.bytes=3;assert(!probe_read_valid(&read));
    read.bytes=4;read.space=RM443_PROBE_READ_BRIDGE;assert(probe_read_valid(&read));
    read.space=2;assert(!probe_read_valid(&read)); read.space=0;read.reserved=1;assert(!probe_read_valid(&read));
    read.reserved=0;read.data=1ULL<<32;assert(!probe_read_valid(&read));
    assert(probe_read_count(8,5,0x1000,4)==4);
    assert(probe_read_count(4,1,0x1004,4)==0);
    assert(probe_read_count(8,8,0x1ffc,4)==4);

    const void *sites[]={rm443_probe_load8,rm443_probe_load16,rm443_probe_load32};
    for(unsigned i=0;i<3;i++) {
        struct pt_regs regs={.pc=(unsigned long)sites[i]}; assert(!probe_external_abort(0,8,&regs));
        regs.user=true;assert(probe_external_abort(0,8,&regs)==1);
    }
    assert(fixups==3); struct pt_regs regs={.pc=0x12345678};
    assert(probe_external_abort(0,8,&regs)==1 && fixups==3);
    can_fixup=false;regs.pc=(unsigned long)rm443_probe_load8;assert(probe_external_abort(0,8,&regs)==1);
    assert(!alive && !maps_alive && !s.charged && !held);
    return 0;
}
'''


class ProbeDriver(unittest.TestCase):
    def test_paging_cow_budget_rollback_and_precise_faults(self):
        compiler=shutil.which("cc")
        self.assertIsNotNone(compiler)
        source=SOURCE.read_text()
        structs=source[source.index("struct probe_buffer {"):source.index("static atomic_t")]
        uapi=re.sub(r"^#include.*$","",ABI.read_text(),flags=re.MULTILINE)
        names=("probe_find","probe_buffer_range","probe_room","probe_page_physical","probe_mapping_page",
               "probe_owned_entry","probe_unmap_buffer","probe_drop_buffer","probe_object_valid","probe_new_buffer",
               "probe_object_description","probe_alloc","probe_object","probe_copy_out","probe_copy_in",
               "probe_budget","probe_read_valid","probe_read_count","probe_external_abort")
        bodies="\n".join(function(source,name) for name in names)
        with tempfile.TemporaryDirectory(prefix="rm443-probe-driver-") as directory:
            root=Path(directory)
            path,binary=root/"driver.c",root/"driver"
            path.write_text(HARNESS+uapi+structs+bodies+MAIN)
            subprocess.run([compiler,"-std=gnu11","-O1","-Wall","-Wextra","-Werror","-Wno-unused-parameter",
                            "-fsanitize=address,undefined",str(path),"-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True)


if __name__=="__main__":
    unittest.main()
