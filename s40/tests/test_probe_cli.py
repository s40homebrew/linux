#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-test the real client against an ioctl fixture; never open a device."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
from paths import KERNEL_ROOT

FIXTURE = r'''
#include <assert.h>
#include <stdarg.h>
#include <sys/wait.h>
#define open fixture_open
#define ioctl fixture_ioctl
#define main client_main
#include "CLIENT_SOURCE"
#undef main
#undef ioctl
#undef open
int open(const char *, int, ...);
_Static_assert(sizeof(struct rm443_probe_info) == 64, "info ABI");
_Static_assert(sizeof(struct rm443_probe_buffer) == 16, "buffer ABI");
_Static_assert(sizeof(struct rm443_probe_transfer) == 24, "transfer ABI");
_Static_assert(sizeof(struct rm443_probe_call) == 1256, "call ABI");
_Static_assert(sizeof(struct rm443_probe_read) == 40, "read ABI");
_Static_assert(sizeof(struct rm443_probe_object) == 48, "object ABI");
_Static_assert(sizeof(struct rm443_probe_page) == 16, "page ABI");
_Static_assert(sizeof(struct rm443_probe_pages) == 24, "pages ABI");
_Static_assert(sizeof(struct rm443_probe_budget) == 32, "budget ABI");
_Static_assert(sizeof(struct rm443_probe_copy) == 32, "copy ABI");
static struct rm443_probe_object objects[32];
static unsigned char memory[131072];
int fixture_open(const char *path, int flags, ...) {
    va_list ap; mode_t mode = 0;
    if (flags & O_CREAT) { va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    if (!strcmp(path, "/dev/rm443_probe")) return open("/dev/null", O_RDWR);
    return open(path, flags, mode);
}
static struct rm443_probe_call last;
static unsigned int calls, uploads, downloads, requests;
int fixture_ioctl(int fd, unsigned long op, ...) {
    va_list ap;
    void *p;
    (void)fd;
    requests++;
    va_start(ap, op); p = va_arg(ap, void *); va_end(ap);
    if (op == RM443_PROBE_INFO) {
        struct rm443_probe_info *info = p;
        *info = (struct rm443_probe_info){ .abi = RM443_PROBE_ABI,
            .max_buffer = RM443_PROBE_MAX_BUFFER, .max_transfer = RM443_PROBE_MAX_TRANSFER };
        return 0;
    }
    if (op == RM443_PROBE_OBJECT_CREATE) {
        struct rm443_probe_object *o = p;
        o->id = o->kind == RM443_PROBE_OBJECT_PAGED ? 8 : 9;
        o->page_count = (o->bytes + 4095) / 4096;
        o->resident_bytes = o->kind == RM443_PROBE_OBJECT_PAGED ? o->page_count * 4096 : 0;
        objects[o->id] = *o;
        return 0;
    }
    if (op == RM443_PROBE_OBJECT_INFO || op == RM443_PROBE_MAP || op == RM443_PROBE_UNMAP) {
        struct rm443_probe_object *o = p;
        assert(o->id < 32 && objects[o->id].id);
        if (op == RM443_PROBE_MAP) { objects[o->id].bridge_va = 0x60000000; objects[o->id].mapped_bytes = objects[o->id].page_count * 4096; }
        if (op == RM443_PROBE_UNMAP) { objects[o->id].bridge_va = 0; objects[o->id].mapped_bytes = 0; }
        *o = objects[o->id];
        return 0;
    }
    if (op == RM443_PROBE_PAGES) {
        struct rm443_probe_pages *list = p;
        struct rm443_probe_page *out = (void *)(uintptr_t)list->data;
        assert(list->count <= 256);
        for (uint32_t i = 0; i < list->count; i++)
            out[i] = (struct rm443_probe_page){ .offset = (list->start+i)*4096,
                .physical = 0x11000000 + (list->start+i)*8192, .kind = RM443_PROBE_PAGE_RAM };
        return 0;
    }
    if (op == RM443_PROBE_BUDGET) {
        struct rm443_probe_budget *b = p;
        if (!b->flags) { b->limit_bytes = RM443_PROBE_MAX_TOTAL; b->headroom_bytes = RM443_PROBE_MIN_HEADROOM; }
        b->max_object_bytes = RM443_PROBE_MAX_OBJECT;
        b->max_contiguous_bytes = RM443_PROBE_MAX_BUFFER;
        b->available_bytes = 16*1024*1024;
        return 0;
    }
    if (op == RM443_PROBE_COPY_PHYS) {
        struct rm443_probe_copy *c = p;
        assert(c->bytes <= RM443_PROBE_MAX_TRANSFER);
        memset(memory + c->offset, 0x6b, c->bytes);
        c->done = c->bytes;
        return 0;
    }
    if (op == RM443_PROBE_ALLOC) {
        struct rm443_probe_buffer *b = p;
        assert(b->bytes <= sizeof(memory));
        b->id = 7; b->physical = 0x11000000;
        objects[7] = (struct rm443_probe_object){ .id = 7, .bytes = b->bytes,
            .physical = b->physical, .resident_bytes = (b->bytes+4095)&~4095U,
            .page_count = (b->bytes+4095)/4096 };
        return 0;
    }
    if (op == RM443_PROBE_UPLOAD || op == RM443_PROBE_DOWNLOAD) {
        struct rm443_probe_transfer *x = p;
        assert(x->id >= 7 && x->id <= 9 && x->bytes <= 65536);
        assert((uint64_t)x->offset + x->bytes <= sizeof(memory));
        if (op == RM443_PROBE_UPLOAD) {
            uploads++; memcpy(memory + x->offset, (void *)(uintptr_t)x->data, x->bytes);
        } else {
            downloads++; memcpy((void *)(uintptr_t)x->data, memory + x->offset, x->bytes);
        }
        return 0;
    }
    if (op == RM443_PROBE_CALL) {
        struct rm443_probe_call *c = p;
        last = *c; calls++;
        c->returned[0] = c->regs[0] + c->regs[1];
        c->stage = 30; c->sequence = calls;
        return 0;
    }
    if (op == RM443_PROBE_RECORD) { memcpy(p, &last, sizeof(last)); return 0; }
    if (op == RM443_PROBE_READ) {
        struct rm443_probe_read *r = p;
        r->done = r->width;
        memset((void *)(uintptr_t)r->data, 0x5a, r->done);
        r->status = -EFAULT; r->fault_address = r->address + r->done;
        return 0;
    }
    if (op == RM443_PROBE_FREE) return 0;
    assert(!"unexpected ioctl"); return -1;
}
static int run(struct session *s, const char *line) {
    char text[4096], *words[MAX_WORDS];
    int n;
    assert(strlen(line) < sizeof(text));
    strcpy(text, line); n = split(text, words);
    return n < 0 ? n : command(s, n, words);
}
int main(void) {
    struct session s = {.fd = -1, .info.max_buffer = RM443_PROBE_MAX_BUFFER};
    uint32_t n;
    unsigned int before;
    FILE *f;
    char text[256];
    assert(number("4294967295", &n) == 0 && n == UINT32_MAX);
    assert(number("0xffffffff", &n) == 0 && n == UINT32_MAX);
    assert(number("010", &n) == 0 && n == 10);
    assert(number("-1", &n) < 0 && number("+1", &n) < 0);
    assert(number("4294967296", &n) < 0 && number(" 1", &n) < 0);
    assert(number("0x", &n) < 0 && number("1garbage", &n) < 0);
    assert(run(&s, "alloc data 131072") == 0);
    assert(address(&s, "@data+0x20", &n) == 0 && n == 0x11000020);
    assert(address(&s, "@data+131072", &n) < 0);
    assert(address(&s, "@absent", &n) < 0);
    before = requests;
    assert(run(&s, "alloc data 2") < 0);
    assert(run(&s, "alloc 9invalid 2") < 0);
    assert(run(&s, "alloc wrong 4294967296") < 0);
    assert(run(&s, "exec @data r13=1") < 0);
    assert(run(&s, "exec @data r0=1 r0=2") < 0);
    assert(run(&s, "exec @data+2") < 0);
    assert(run(&s, "call descriptor=1,2,3") < 0);
    assert(run(&s, "call descriptor=1,2,3,4 r0=1") < 0);
    assert(run(&s, "call descriptor=1,2,3,4 stack=1,") < 0);
    assert(run(&s, "call descriptor=1,2,3,4 stack=0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16") < 0);
    assert(run(&s, "read physical 1 2 4") < 0);
    assert(run(&s, "read physical 0 3 6") < 0);
    assert(run(&s, "read physical 0xfffffffc 4 8") < 0);
    assert(requests == before);
    assert(run(&s, "exec @data+1 r0=7 r1=11 r12=0xffffffff stack=@data+32,2") == 0);
    assert(last.entry == 0x11000001 && last.regs[0] == 7 && last.regs[1] == 11);
    assert(last.regs[12] == UINT32_MAX && last.stack_words == 2);
    assert(last.stack[0] == 0x11000020 && !last.flags);
    assert(run(&s, "call descriptor=1,2,3,4 r1=0x1f r2=@data stack=@data+8,@data+16,2") == 0);
    assert(last.flags == RM443_PROBE_CALL_ROM && last.entry == RM443_PROBE_ROM_ENTRY);
    assert(last.descriptor[3] == 4 && last.stack[1] == 0x11000010);
    assert(run(&s, "call descriptor=1,2,3,4 entry=0xffff0811") == 0);
    assert(last.entry == 0xffff0811);
    f = fopen("descriptor.bin", "wb"); assert(f);
    for (int i = 0; i < 16; i++) assert(fputc(i, f) == i);
    assert(!fclose(f));
    assert(run(&s, "call descriptor=file:descriptor.bin") == 0);
    assert(last.descriptor[0] == 0x03020100 && last.descriptor[3] == 0x0f0e0d0c);
    f = fopen("descriptor.bin", "ab"); assert(f); fputc(0, f); fclose(f);
    before = calls;
    assert(run(&s, "call descriptor=file:descriptor.bin") < 0 && calls == before);
    f = fopen("input with spaces.bin", "wb"); assert(f);
    for (int i = 0; i < 65537; i++) assert(fputc(i & 255, f) == (i & 255));
    assert(!fclose(f));
    assert(run(&s, "upload data 'input with spaces.bin' 4 # comment") == 0);
    assert(uploads == 2 && memory[4] == 0 && memory[65540] == 0);
    assert(run(&s, "download data output.bin 4 65537") == 0 && downloads == 2);
    f = fopen("output.bin", "rb"); assert(f);
    for (int i = 0; i < 65537; i++) assert(fgetc(f) == (i & 255));
    assert(fgetc(f) == EOF); fclose(f);
    assert(run(&s, "download data output.bin 4 1") < 0);
    assert(run(&s, "read physical 0xffff0800 2 16 fault.bin") < 0);
    f = fopen("fault.bin", "rb"); assert(f);
    assert(fgetc(f) == 0x5a && fgetc(f) == 0x5a && fgetc(f) == EOF); fclose(f);
    assert(run(&s, "upload data 'unclosed") < 0);
    assert(run(&s, "free data") == 0 && !named(&s, "data"));
    assert(run(&s, "# just a comment") == 0);
    assert(run(&s, "quit") == 1);
    strcpy(text, "exec 1 ");
    for (int i = 0; i < 40; i++) strcat(text, "a ");
    assert(run(&s, text) < 0);
    assert(run(&s, "paged image 20971520") == 0);
    assert(named(&s, "image")->object.kind == RM443_PROBE_OBJECT_PAGED);
    assert(address(&s, "@image", &n) < 0 && address(&s, "%image", &n) < 0);
    assert(run(&s, "map image") == 0);
    assert(address(&s, "%image+32", &n) == 0 && n == 0x60000020);
    before = calls;
    assert(run(&s, "call descriptor=1,2,3,4 r2=%image") < 0);
    assert(run(&s, "call descriptor=1,2,3,4 stack=%image") < 0);
    assert(run(&s, "call descriptor=1,2,3,%image") < 0 && calls == before);
    assert(run(&s, "read physical %image 1 4") < 0);
    assert(run(&s, "exec %image+1 r0=%image+32") == 0);
    assert(last.entry == 0x60000001 && last.flags == RM443_PROBE_CALL_VIRTUAL);
    assert(last.regs[0] == 0x60000020);
    assert(run(&s, "execva 0x60000000") == 0 && last.flags == RM443_PROBE_CALL_VIRTUAL);
    assert(run(&s, "alloc code 4096") == 0);
    assert(run(&s, "exec @code r0=%image") == 0 && !last.flags && last.regs[0] == 0x60000000);
    assert(run(&s, "pages image") == 0); /* spans twenty 256-entry requests */
    assert(run(&s, "unmap image") == 0 && address(&s, "%image", &n) < 0);
    assert(run(&s, "flash stock 0x1000 131072") == 0);
    assert(!named(&s, "stock")->object.resident_bytes);
    assert(address(&s, "@stock", &n) < 0);
    assert(run(&s, "flash bad 1 4096") < 0);
    assert(run(&s, "flash bad 0x3fff000 8192") < 0);
    assert(run(&s, "puthex stock 4095 0102aBfF") == 0);
    assert(memory[4095] == 1 && memory[4096] == 2 && memory[4097] == 0xab && memory[4098] == 255);
    before = uploads;
    assert(run(&s, "puthex stock 0 0") < 0);
    assert(run(&s, "puthex stock 0 aZ") < 0 && uploads == before);
    assert(run(&s, "gethex stock 4095 4") == 0);
    assert(run(&s, "copy stock 8 0x1000 65537") == 0);
    assert(memory[8] == 0x6b && memory[65544] == 0x6b);
    assert(run(&s, "budget") == 0);
    assert(run(&s, "budget 8388608 4194304") == 0);
    assert(run(&s, "budget 8388608 4194303") < 0);
    s.input = tmpfile(); assert(s.input);
    for (int i=0; i<65537; i++) assert(fputc(i & 255, s.input) == (i & 255));
    fputs("next command\n", s.input); rewind(s.input);
    before = uploads;
    assert(run(&s, "import stock - 0 65537") == 0 && uploads == before + 2);
    assert(fgets(text, sizeof(text), s.input) && !strcmp(text, "next command\n"));
    fclose(s.input); s.input = NULL;
    assert(run(&s, "export stock streamed.bin 0 65537") == 0);
    f = fopen("streamed.bin", "rb"); assert(f);
    for (int i=0; i<65537; i++) assert(fgetc(f) == (i & 255));
    assert(fgetc(f) == EOF); fclose(f);
    assert(run(&s, "export stock streamed.bin 0 1") < 0);
    assert(run(&s, "export stock - 0 1") < 0);
    f = fopen("short.bin", "wb"); assert(f); fputs("x", f); fclose(f);
    before = uploads;
    assert(run(&s, "import stock short.bin 0 2") < 0 && uploads == before);
    assert(!mkfifo("input.pipe", 0600));
    pid_t child = fork(); assert(child >= 0);
    if (!child) { FILE *out = fopen("input.pipe", "wb"); if (!out) _exit(2);
        for (int i=0;i<257;i++) { if (fputc(i&255,out) == EOF || fflush(out)) _exit(3); }
        _exit(fclose(out) ? 4 : 0); }
    assert(run(&s, "import stock input.pipe 0 257") == 0);
    int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(memory[255] == 255 && memory[256] == 0);
    /* Real main, fake device: --binary must keep every status line off stdout. */
    f=fopen("binary.probe","wb"); assert(f);
    fputs("alloc out 16\nputhex out 0 000102ff\nexport out - 0 4\nquit\n",f); fclose(f);
    fflush(stdout); int saved_stdout=dup(STDOUT_FILENO);
    int raw=open("raw.bin",O_WRONLY|O_CREAT|O_EXCL,0600); assert(raw>=0);
    assert(dup2(raw,STDOUT_FILENO)>=0); close(raw);
    char *args[]={"rmprobe","--binary","-f","binary.probe",NULL};
    assert(client_main(4,args)==0); fflush(stdout);
    assert(dup2(saved_stdout,STDOUT_FILENO)>=0); close(saved_stdout); status_output=stdout;
    f=fopen("raw.bin","rb"); assert(f);
    assert(fgetc(f)==0 && fgetc(f)==1 && fgetc(f)==2 && fgetc(f)==255 && fgetc(f)==EOF); fclose(f);
    /* --fifo keeps a session across independent writes and unlinks only
     * its owned FIFO on a normal quit or command failure. */
    for (int bad = 0; bad < 2; bad++) {
        fflush(stdout);
        child = fork(); assert(child >= 0);
        if (!child) {
            char *fifo_args[] = {"rmprobe", "--fifo", "command.pipe", NULL};
            _exit(client_main(3, fifo_args));
        }
        struct stat st;
        int ready = 0;
        for (int i=0; i<1000; i++) {
            if (!lstat("command.pipe",&st)) { ready=1; break; }
            usleep(1000);
        }
        assert(ready && S_ISFIFO(st.st_mode));
        FILE *writer = fopen("command.pipe", "w"); assert(writer);
        if (bad) fputs("bad-command\n", writer);
        else {
            fputs("alloc buffer 4096\n", writer); assert(!fflush(writer));
            fputs("puthex buffer 0 01020304\nquit\n", writer);
        }
        assert(!fclose(writer));
        assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==bad);
        assert(lstat("command.pipe",&st)<0 && errno==ENOENT);
    }
    assert(!mkfifo("existing.pipe",0600));
    char *existing_args[]={"rmprobe","--fifo","existing.pipe",NULL};
    assert(client_main(3,existing_args)==1);
    struct stat existing; assert(!lstat("existing.pipe",&existing) && S_ISFIFO(existing.st_mode));
    puts("client fixture passed");
    return 0;
}
'''


class ProbeClientTests(unittest.TestCase):
    def test_real_client_with_mock_ioctls(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "linux").mkdir()
            (root / "sys").mkdir()
            (root / "sys/ioctl.h").write_text("int ioctl(int, unsigned long, ...);\n")
            (root / "linux/types.h").write_text(
                "#include <stdint.h>\n"
                "typedef uint32_t __u32; typedef int32_t __s32;\n"
                "#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n"
            )
            # Use Linux's ioctl encoding even on a Darwin host.
            (root / "linux/ioctl.h").write_text(
                f'#include "{KERNEL_ROOT}/include/uapi/asm-generic/ioctl.h"\n'
            )
            source = FIXTURE.replace("CLIENT_SOURCE", str(ROOT / "kernel/userspace/rmprobe.c"))
            (root / "fixture.c").write_text(source)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", tmp, str(root / "fixture.c"), "-o", str(root / "fixture")], check=True)
            subprocess.run([str(root / "fixture")], cwd=root, check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
