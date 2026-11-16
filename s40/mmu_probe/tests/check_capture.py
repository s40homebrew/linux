#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute compiled ARM capture code against synthetic mappings.

CP15 accesses are modeled; this validates transactions and address arithmetic,
not physical MMU/domain timing. No firmware image is needed.
"""
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn import arm_const as arm

PROJECT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT / "tools"))
from port_paths import llvm_tool
def build_capture():
    global segments, symbols
    BUILD = PROJECT / "build" / "mmu_probe" / "capture_test"
    BUILD.mkdir(parents=True, exist_ok=True)
    CC = os.environ.get("CC") or llvm_tool("clang")
    LD = os.environ.get("LD") or llvm_tool("ld.lld") or shutil.which("ld.lld")
    if not CC or not LD:
        raise SystemExit("LLVM Clang and LLD are required; set LLVM, CC or LD")
    subprocess.run([CC, "-target", "arm-none-eabi", "-march=armv5te", "-marm",
                    "-O2", "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-pic",
                    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-c",
                    str(PROJECT / "boot_probe/native/capture.c"), "-o", str(BUILD / "capture.o")], check=True)
    subprocess.run([LD, "-Ttext=0x100000", "-e", "linux6303_capture",
                    str(BUILD / "capture.o"), "-o", str(BUILD / "capture.elf")], check=True)
    with (BUILD / "capture.elf").open("rb") as stream:
        elf = ELFFile(stream)
        segments = [(p["p_vaddr"], p.data(), p["p_memsz"]) for p in elf.iter_segments() if p["p_type"] == "PT_LOAD"]
        symbols = {s.name: (s["st_value"], s["st_size"]) for s in elf.get_section_by_name(".symtab").iter_symbols()}

REGS = [getattr(arm, f"UC_ARM_REG_R{i}") for i in range(13)] + [arm.UC_ARM_REG_SP, arm.UC_ARM_REG_LR, arm.UC_ARM_REG_PC]
SNAP = 0xd8100000
STACK = 0xd820f000
STOP = 0x900000
L1_OFF = 52
TARGET_OFF = 16436
TARGET_SIZE = 4124
SNAP_SIZE = TARGET_OFF + 6 * TARGET_SIZE
TTBR = 0x101c0000
CPSR = 0xa000011f
CONTROL = 0x0005107d
DOMAINS = 0x14


def words(*values):
    return struct.pack("<" + "I" * len(values), *values)


def machine():
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    pages = set()
    for address, data, size in segments:
        for page in range(address & ~4095, (address + size + 4095) & ~4095, 4096):
            if page not in pages:
                uc.mem_map(page, 4096)
                pages.add(page)
        uc.mem_write(address, data)
    for address, size in [(SNAP, 0x10000), (0xd8200000, 0x10000),
                          (0xd0000000, 0x100000), (0xf0100000, 0x11000), (STOP, 4096)]:
        uc.mem_map(address, size)
    state = {(0, 0, 0): 0x41069265, (0, 0, 1): 0x1d152152,
             (1, 0, 0): CONTROL, (2, 0, 0): TTBR, (3, 0, 0): DOMAINS, (13, 0, 0): 0}
    changes = []

    def instruction(u, address, size, _):
        if address == STOP:
            u.emu_stop()
            return
        opcode = struct.unpack("<I", u.mem_read(address, 4))[0]
        if opcode & 0x0f000f10 != 0x0e000f10:
            return
        key = ((opcode >> 16) & 15, opcode & 15, (opcode >> 5) & 7)
        reg = REGS[(opcode >> 12) & 15]
        if opcode & (1 << 20):
            assert key in state, (hex(address), key)
            u.reg_write(reg, state[key])
        else:
            value = u.reg_read(reg)
            assert u.reg_read(arm.UC_ARM_REG_CPSR) & 0xc0 == 0xc0
            assert key in ((3, 0, 0), (7, 10, 4)), key
            if key == (3, 0, 0):
                changes.append(value)
                state[key] = value
        u.reg_write(arm.UC_ARM_REG_PC, address + 4)

    def table_read(u, access, address, size, value, _):
        if 0xd0000000 <= address < 0xd0100000:
            assert u.reg_read(arm.UC_ARM_REG_CPSR) & 0xc0 == 0xc0
            assert state[(3, 0, 0)] & 3 == 1

    def no_table_write(u, access, address, size, value, _):
        assert not 0xd0000000 <= address < 0xd0100000

    uc.hook_add(UC_HOOK_CODE, instruction)
    uc.hook_add(UC_HOOK_MEM_READ, table_read)
    uc.hook_add(UC_HOOK_MEM_WRITE, no_table_write)
    return uc, state, changes


def call(uc, name, *args):
    uc.reg_write(arm.UC_ARM_REG_CPSR, CPSR)
    uc.reg_write(arm.UC_ARM_REG_SP, STACK)
    uc.reg_write(arm.UC_ARM_REG_LR, STOP)
    for i, value in enumerate(args):
        uc.reg_write(REGS[i], value)
    uc.emu_start(symbols[name][0], STOP + 4, count=4000000)
    assert uc.reg_read(arm.UC_ARM_REG_PC) == STOP, "instruction limit or unexpected return"
    return uc.reg_read(arm.UC_ARM_REG_R0)


def cpu(uc):
    uc.mem_write(SNAP, words(CPSR, 0x41069265, 0x1d152152, CONTROL, TTBR, DOMAINS, 0, 0, 0))


def mapped_tables():
    l1 = [0x72] * 4096
    l1[0xd80] = 0x10400c22
    l1[0xd81] = (TTBR + 0x4000) | 0x21
    l1[0xd82] = (TTBR + 0x5000) | 0x23
    l1[0xd83] = (TTBR + 0x6000) | 0x21
    l1[0xd84] = 0
    l1[0xd85] = (TTBR + 0x7000) | 0x21
    small = [(0x10500000 + 4096 * i) | 0xff2 for i in range(256)]
    fine = [(0x10600000 + 1024 * i) | 0x33 for i in range(1024)]
    large = [(0x10700000 + 65536 * (i // 16)) | 0xff1 for i in range(256)]
    bad = [0x10800003] * 256
    return l1, [small, fine, large, bad]


def test_capture():
    uc, state, changes = machine()
    cpu(uc)
    l1, tables = mapped_tables()
    uc.mem_write(0xd0000000, words(*l1))
    for address, table in zip((0xd0004000, 0xd0005000, 0xd0006000, 0xd0007000), tables):
        uc.mem_write(address, words(*table))
    vas = [0xd8001234, 0xd810fedc, 0xd8200010, 0xd8300812, 0xd8400034, 0xd8500404]
    for i, va in enumerate(vas):
        uc.mem_write(SNAP + TARGET_OFF + TARGET_SIZE * i, words(va))
    call(uc, "linux6303_capture", SNAP)
    assert bytes(uc.mem_read(SNAP + L1_OFF, 16384)) == words(*l1)
    expected = [(0x10401234, 0), (0x1050fedc, 0), (0x10600010, 0), (0x10700812, 0), (0, 2), (0, 4)]
    for i, (pa, status) in enumerate(expected):
        result = struct.unpack("<7I", uc.mem_read(SNAP + TARGET_OFF + TARGET_SIZE * i, 28))
        assert (result[3], result[4]) == (pa, status), result
    assert state[(3, 0, 0)] == DOMAINS and changes == [DOMAINS | 1, DOMAINS]
    assert uc.reg_read(arm.UC_ARM_REG_CPSR) & 255 == CPSR & 255
    print("capture: L1, coarse/fine, small/large/tiny, faults, no table writes, DACR and masks restored")


def test_ranges():
    for corrupt in (False, True):
        uc, state, changes = machine()
        cpu(uc)
        l1 = [0x72] * 4096
        for i in range(6):
            l1[0xd90 + i] = (0x11000000 + 0x100000 * i) | 0xc22
        if corrupt:
            l1[0xd91] += 0x100000
        uc.mem_write(0xd0000000, words(*l1))
        call(uc, "linux6303_translate_range", SNAP, 0xd9000000, 6 * 1024 * 1024, SNAP + 0xe000)
        result = struct.unpack("<6I", uc.mem_read(SNAP + 0xe000, 24))
        assert result[2] == 0x11000000
        assert result[3] == (0x100000 if corrupt else 0x600000), result
        assert result[5] == (6 if corrupt else 0), result
        assert state[(3, 0, 0)] == DOMAINS and changes == [DOMAINS | 1, DOMAINS]
        assert uc.reg_read(arm.UC_ARM_REG_CPSR) & 255 == CPSR & 255
    uc, state, changes = machine()
    cpu(uc)
    state[(2, 0, 0)] += 0x4000
    call(uc, "linux6303_capture", SNAP)
    assert struct.unpack("<I", uc.mem_read(SNAP + 40, 4))[0] == 1
    assert changes == [] and uc.reg_read(arm.UC_ARM_REG_CPSR) & 255 == CPSR & 255
    print("range: 6MiB contiguity, discontinuity rejection, changed-TTBR refusal and state restoration")


if __name__ == "__main__":
    build_capture()
    test_capture()
    test_ranges()
