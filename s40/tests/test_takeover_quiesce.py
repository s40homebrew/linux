#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute the physical takeover routine, including bounded hardware failures."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from unicorn import UC_ARCH_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc
from unicorn.arm_const import (
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_R0,
    UC_ARM_REG_R1,
    UC_ARM_REG_R2,
    UC_ARM_REG_R3,
    UC_ARM_REG_R8,
    UC_ARM_REG_R9,
    UC_ARM_REG_R10,
    UC_ARM_REG_R11,
    UC_ARM_REG_SP,
)

PROJECT = Path(__file__).resolve().parents[1]
DSP = 0x20032D18
LINEAR = [0x20040C08 + channel * 0x400 for channel in range(6)]
LINEAR_ENABLE = [address - 4 for address in LINEAR]
CIRCULAR = [0x20042440 + channel * 4 for channel in range(4)]
USB = [0x20036628, 0x2003662C, 0x20036640, 0x20036644]
USB_LINK = [0x20036618, 0x20036678]
WATCHDOG = 0x20040504


from paths import llvm_tool


class TakeoverQuiesce(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not all(llvm_tool(name) for name in ("clang", "ld.lld", "llvm-objcopy")):
            raise unittest.SkipTest("LLVM ARM tools required")
        cls.temporary = tempfile.TemporaryDirectory(prefix='rm443-quiesce-test-')
        cls.directory = Path(cls.temporary.name)
        assembly = cls.directory / 'quiesce.S'
        assembly.write_text('.syntax unified\n.arch armv5te\n.arm\n.text\n'
                            '#include "rm443_quiesce.inc"\n')
        subprocess.run([llvm_tool('clang'), '-target', 'arm-none-eabi', '-c',
                        '-I', str(PROJECT / 'handoff'), str(assembly),
                        '-o', str(cls.directory / 'quiesce.o')], check=True)
        subprocess.run([llvm_tool('ld.lld'), '--image-base=0', '-Ttext=0x1000', '-e', 'rm443_quiesce',
                        str(cls.directory / 'quiesce.o'), '-o', str(cls.directory / 'quiesce.elf')], check=True)
        subprocess.run([llvm_tool('llvm-objcopy'), '-O', 'binary',
                        str(cls.directory / 'quiesce.elf'), str(cls.directory / 'quiesce.bin')], check=True)
        cls.code = (cls.directory / 'quiesce.bin').read_bytes()

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def execute(self, fault=None):
        cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        cpu.mem_map(0x1000, 0x2000)
        cpu.mem_write(0x1000, self.code)
        cpu.mem_map(0x20000000, 0x50000)
        before = 0xA5A50041
        cpu.mem_write(DSP, before.to_bytes(4, 'little'))
        for address in LINEAR + LINEAR_ENABLE + CIRCULAR + USB + USB_LINK:
            cpu.mem_write(address, (0xFFFF).to_bytes(4, 'little'))
        writes, reads = [], {}

        def read(uc, access, address, size, value, user):
            reads[address] = reads.get(address, 0) + 1
            if address == fault and (address != DSP or reads[address] > 1):
                uc.mem_write(address, (1).to_bytes(size, 'little'))

        def write(uc, access, address, size, value, user):
            writes.append((address, size, value))
            self.assertIn(address, [DSP, WATCHDOG] + LINEAR + LINEAR_ENABLE + CIRCULAR + USB + USB_LINK)
            self.assertEqual(size, 2 if address in USB + USB_LINK else 4)
            if address in LINEAR_ENABLE:
                self.assertGreater(reads.get(address + 4, 0), 0)
                self.assertEqual(int.from_bytes(uc.mem_read(address + 4, 4), 'little'), 0,
                                 'linear enable cleared before command became idle')

        cpu.hook_add(UC_HOOK_MEM_READ, read)
        cpu.hook_add(UC_HOOK_MEM_WRITE, write)
        saved = [UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11]
        for register in saved:
            cpu.reg_write(register, 0x12340000 + register)
        # An unmapped stack proves the routine has no dependency on old S40 RAM.
        cpu.reg_write(UC_ARM_REG_SP, 0xFFFFFFFF)
        cpu.reg_write(UC_ARM_REG_LR, 0x2000)
        cpu.emu_start(0x1000, 0x2000, count=2000000)
        self.assertEqual(cpu.reg_read(UC_ARM_REG_PC), 0x2000, 'routine failed to return within bounded instruction count')
        for register in saved:
            self.assertEqual(cpu.reg_read(register), 0x12340000 + register)
        self.assertEqual(cpu.reg_read(UC_ARM_REG_R3), before)
        self.assertEqual(sum(a == WATCHDOG for a, _, _ in writes), 2)
        values = [cpu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2)]
        return cpu, values, writes

    def test_success_preserves_unknown_dsp_bits_and_stops_all_requests(self):
        cpu, (status, dsp_after, completed), writes = self.execute()
        self.assertEqual((status, dsp_after, completed), (0, 0xA5A50040, 15))
        for address in LINEAR + LINEAR_ENABLE + CIRCULAR + USB:
            size = 2 if address in USB else 4
            self.assertEqual(int.from_bytes(cpu.mem_read(address, size), 'little'), 0)
        self.assertEqual(int.from_bytes(cpu.mem_read(USB_LINK[0], 2), 'little'), 0xFFFE)
        self.assertEqual(int.from_bytes(cpu.mem_read(USB_LINK[1], 2), 'little'), 0x7FFF)
        self.assertEqual([a for a, _, _ in writes if a in LINEAR + CIRCULAR], LINEAR + CIRCULAR)
        self.assertEqual([a for a, _, _ in writes if a in LINEAR + LINEAR_ENABLE],
                         [a for pair in zip(LINEAR, LINEAR_ENABLE) for a in pair])

    def test_stuck_dsp_control_returns_before_dma_or_usb(self):
        _, (status, _, completed), writes = self.execute(DSP)
        self.assertEqual((status, completed), (0x301, 0))
        self.assertFalse(any(a in LINEAR + CIRCULAR + USB for a, _, _ in writes))

    def test_first_and_last_stuck_linear_channels_stop_takeover(self):
        for channel in (0, len(LINEAR) - 1):
            address = LINEAR[channel]
            with self.subTest(channel=channel):
                _, (status, _, completed), writes = self.execute(address)
                self.assertEqual((status, completed), (0x101 + channel, 1))
                self.assertFalse(any(a == address - 4 for a, _, _ in writes))
                self.assertFalse(any(a in CIRCULAR + USB for a, _, _ in writes))

    def test_first_and_last_stuck_circular_channels_stop_takeover(self):
        for channel in (0, len(CIRCULAR) - 1):
            address = CIRCULAR[channel]
            with self.subTest(channel=channel):
                _, (status, _, completed), writes = self.execute(address)
                self.assertEqual((status, completed), (0x201 + channel, 3))
                self.assertFalse(any(a in USB for a, _, _ in writes))

    def test_stuck_usb_request_or_interrupt_disable_fails(self):
        for address in (USB[0], USB[-1]):
            with self.subTest(address=hex(address)):
                _, (status, _, completed), _ = self.execute(address)
                self.assertEqual((status, completed), (0x401, 7))


if __name__ == '__main__':
    unittest.main()
