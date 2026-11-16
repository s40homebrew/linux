# SPDX-License-Identifier: MIT
"""Exercise actual MMC helpers against a FIFO that holds EOC until primed.

SPRU765A figure 20 requests TX data before end-of-command; the generic OMAP
driver services FIFO requests while the command is pending. The model also
withholds EOC on CARD_ERR, as required by the controller status contract.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from support import MACH, function

DRIVER = MACH / "rm443_mmc.c"


class MmcWrite(unittest.TestCase):
    def test_command_fifo_and_data_handoff(self):
        source = DRIVER.read_text()
        defines = source[source.index("#define MMC_CMD"):source.index("struct rm443_mmc {")]
        helpers = "\n".join(function(source, name) for name in (
            "rm443_read", "rm443_write", "rm443_busy", "rm443_data_timeout_ms", "rm443_write_word",
            "rm443_command", "rm443_data"))
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1U << (n))
#define MMC_RSP_136 BIT(0)
#define MMC_RSP_PRESENT BIT(1)
#define MMC_RSP_CRC BIT(2)
#define MMC_RSP_BUSY BIT(3)
#define MMC_CMD_BC 0
#define MMC_CMD_BCR 1
#define MMC_CMD_ADTC 3
#define MMC_DATA_READ 1
#define SD_SEND_IF_COND 8
#define SD_SEND_RELATIVE_ADDR 3
#define mmc_cmd_type(cmd) ((cmd)->type)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define DIV_ROUND_UP_ULL(n, d) DIV_ROUND_UP(n, d)
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define clamp(n, a, b) ((n) < (a) ? (a) : ((n) > (b) ? (b) : (n)))
#define msecs_to_jiffies(n) (n)
#define mmc_dev(mmc) (mmc)
#define dev_err_ratelimited(...) ((void)0)
#define dev_info(...) ((void)0)
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (0)
struct mmc_command { unsigned opcode, flags, type, arg, busy_timeout; int error; u32 resp[4]; };
struct mmc_data { unsigned blksz, blocks, flags, timeout_clks, timeout_ns, sg_len, bytes_xfered; void *sg; int error; };
struct mmc_request { struct mmc_data *data; };
struct rm443_mmc { void *mmc; u8 *io, *buffer; u32 clock_hz; struct mmc_request *mrq; bool trace_read; };
static u8 regs[128], payload[32768], received[32768];
static unsigned count, expected, mode, jiffies, command_active;
static u32 load(const void *p) { u32 n; memcpy(&n, p, 4); return n; }
static void store(void *p, u32 n) { memcpy(p, &n, 4); }
static u32 readl(const void *p) { return load(p); }
static void writel(u32 n, void *p) {
    if (p == regs + 0x10) store(p, load(p) & ~n);
    else store(p, n);
    if (p == regs) command_active = 1;
    if (p == regs + 0x20) {
        if (count < sizeof(received)) received[count++] = n;
        if (count < sizeof(received)) received[count++] = n >> 8;
        if (mode == 6) jiffies = (count * 8 * 1000 + 399999) / 400000;
    }
}
static void cond_resched(void) {}
static unsigned sg_copy_from_buffer(void *sg, unsigned n, void *buf, unsigned len) {
    (void)sg; (void)n; (void)buf; return len;
}
static int rm443_wait(struct rm443_mmc *host, u16 mask, unsigned long deadline) {
    (void)host;
    if (jiffies > deadline) return -ETIMEDOUT;
    u16 status = 0;
    if (mode == 1) status = BIT(14); /* Card rejects command, never EOC. */
    else if (mode == 2) status = 0; /* No response and no FIFO readiness. */
    else if (command_active) {
        if (count < expected && (!(mask & BIT(0)) || count < 8 || mode == 6)) status |= BIT(11);
        if (count >= 8) status |= BIT(0); /* clock held until primed */
        if (mode == 4) status |= BIT(0); /* premature EOC with initial AE */
        if (count == expected) status |= BIT(3) | BIT(4);
        if (mode == 3 && count >= 16) status |= BIT(6);
        if (mode == 5 && count >= 2) status |= BIT(6);
    }
    store(regs + 0x10, load(regs + 0x10) | status);
    return status & mask ? status : -ETIMEDOUT;
}
'''
        main = r'''
int main(void) {
    struct mmc_data data = { .blksz = 512, .blocks = 1 };
    struct mmc_request mrq = { .data = &data };
    struct rm443_mmc host = { .io = regs, .buffer = payload, .clock_hz = 12000000, .mrq = &mrq };
    for (unsigned i = 0; i < sizeof(payload); ++i) payload[i] = (i * 37 + (i >> 8)) & 255;
    for (unsigned blocks = 1; blocks <= 64; blocks *= 8) {
        struct mmc_command cmd = { .opcode = blocks == 1 ? 24 : 25,
            .flags = MMC_RSP_PRESENT | MMC_RSP_CRC, .type = MMC_CMD_ADTC };
        unsigned written = 0;
        count = mode = 0; expected = data.blksz * (data.blocks = blocks);
        CHECK(rm443_command(&host, &cmd, false, &written) == 0);
        CHECK(written >= 8 && written < expected);
        CHECK(rm443_data(&host, &data, written) == 0);
        CHECK(count == expected && data.bytes_xfered == expected);
        CHECK(!memcmp(payload, received, expected));
    }
    struct mmc_command cmd = { .opcode = 24, .flags = MMC_RSP_PRESENT | MMC_RSP_CRC, .type = MMC_CMD_ADTC };
    unsigned written = 0;
    mode = 1; count = 0; store(regs + 0x58, 0x0080); store(regs + 0x5c, 0x0400);
    CHECK(rm443_command(&host, &cmd, false, &written) == -EIO);
    CHECK(count == 0 && written == 0 && cmd.resp[0] == 0x04000080);
    mode = 2; written = 0;
    CHECK(rm443_command(&host, &cmd, false, &written) == -ETIMEDOUT);
    mode = 4; count = written = 0;
    CHECK(rm443_command(&host, &cmd, false, &written) == 0);
    CHECK(written == 8); /* Do not accept EOC while AE needs servicing. */
    mode = 5; count = written = 0;
    CHECK(rm443_command(&host, &cmd, false, &written) == -EILSEQ);
    CHECK(written == 2); /* CRC failure before EOC must not be a timeout. */
    mode = 3; count = written = 0; expected = 512; data.blocks = 1;
    CHECK(rm443_command(&host, &cmd, false, &written) == 0);
    CHECK(rm443_data(&host, &data, written) == -EILSEQ);
    CHECK(data.bytes_xfered == 0 && count < expected);
    mode = 6; count = written = jiffies = 0; expected = 32768;
    data.blocks = 64; host.clock_hz = 400000;
    CHECK(rm443_command(&host, &cmd, false, &written) == 0);
    CHECK(jiffies > 500 && written == expected);
    CHECK(rm443_data(&host, &data, written) == 0);
    CHECK(!memcmp(payload, received, expected));
    data.timeout_ns = 0xffffffff; data.timeout_clks = 0xffffffff; host.clock_hz = 1;
    CHECK(rm443_data_timeout_ms(&host, &data) == 60000);
    puts("MMC write: FIFO before EOC, exact payload, 400 kHz timing, bounded timeout and error paths passed");
    return 0;
}
'''
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="rm443-mmc-write-") as temporary:
            path = Path(temporary)
            (path / "write.c").write_text(harness + defines + helpers + main)
            subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            str(path / "write.c"), "-o", str(path / "write")], check=True)
            subprocess.run([str(path / "write")], check=True)


if __name__ == "__main__":
    unittest.main()
