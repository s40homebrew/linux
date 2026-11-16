# SPDX-License-Identifier: MIT
"""Execute the actual stackless ARM payload copier, including destructive overlap."""
import unittest
from support import handoff

from unicorn import UC_ARCH_ARM, UC_MODE_ARM, Uc
from unicorn.arm_const import (
    UC_ARM_REG_CPSR,
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_R0,
    UC_ARM_REG_R1,
    UC_ARM_REG_R2,
    UC_ARM_REG_R3,
    UC_ARM_REG_R7,
    UC_ARM_REG_R8,
    UC_ARM_REG_R9,
    UC_ARM_REG_R10,
    UC_ARM_REG_R11,
    UC_ARM_REG_SP,
)


class PhysicalRelocation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.blob, cls.layout = handoff()

    def run_copy(self, source, destination, image_bytes, memory_bytes):
        code, ram, size = 0x08000000, 0x10000000, 0x40000
        cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        cpu.mem_map(code, 4096)
        cpu.mem_write(code, self.blob)
        cpu.mem_map(ram, size)
        original = bytearray(bytes(range(256)) * (size // 256))
        payload = bytes(((i * 37 + 91) & 255) for i in range(image_bytes))
        original[source:source + image_bytes] = payload
        expected = original.copy()
        expected[destination:destination + image_bytes] = payload
        expected[destination + image_bytes:destination + memory_bytes] = bytes(memory_bytes - image_bytes)
        cpu.mem_write(ram, bytes(original))
        cpu.reg_write(UC_ARM_REG_CPSR, 0xd3)
        cpu.reg_write(UC_ARM_REG_SP, 0xdead0000)  # Deliberately unmapped: no stack allowed.
        for reg, value in ((UC_ARM_REG_R0, ram + source), (UC_ARM_REG_R1, ram + destination),
                           (UC_ARM_REG_R2, image_bytes), (UC_ARM_REG_R3, memory_bytes)):
            cpu.reg_write(reg, value)
        preserved = (UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11)
        for reg in preserved:
            cpu.reg_write(reg, 0xcafe0000 + reg)
        cpu.reg_write(UC_ARM_REG_LR, code + 0xffc)
        cpu.emu_start(code + self.layout["relocate_payload_offset"], code + 0xffc,
                      timeout=5_000_000, count=30_000_000)
        self.assertEqual(cpu.reg_read(UC_ARM_REG_PC), code + 0xffc)
        self.assertEqual(cpu.mem_read(ram, size), expected)
        for reg in preserved:
            self.assertEqual(cpu.reg_read(reg), 0xcafe0000 + reg)
        self.assertEqual(cpu.reg_read(UC_ARM_REG_SP), 0xdead0000)

    def test_overlap_copy_directions_and_bss(self):
        for source, destination, image, memory in (
            (0x12000, 0x10000, 0x3004, 0x3409),  # destination before source
            (0x10000, 0x12000, 0x3007, 0x3403),  # destination inside source
            (0x10000, 0x10000, 0x3003, 0x3407),  # already in place
            (0x10001, 0x30003, 37, 69),            # unaligned non-overlap
            (0x10001, 0x10002, 37, 39),            # unaligned backward
            (0x10002, 0x10001, 37, 40),            # unaligned forward
            (0x20000, 0x10000, 0, 19),             # no file bytes, clear memory
            (0x20000, 0x10000, 3, 3),              # no BSS
        ):
            with self.subTest(source=source, destination=destination, image=image):
                self.run_copy(source, destination, image, memory)

    def test_stackless_image_hash_detects_changed_instruction(self):
        code, data = 0x08000000, 0x10000000
        samples = [b'', b'a', b'foobar', bytes(range(256)) * 16]
        # Real handset corruption: a normal function prologue became a NULL load.
        samples += [bytes.fromhex('f0412de9'), bytes.fromhex('00017b96')]
        hashes = []
        for payload in samples:
            cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
            cpu.mem_map(code, 4096)
            cpu.mem_write(code, self.blob)
            cpu.mem_map(data, 4096)
            if payload:
                cpu.mem_write(data, payload)
            cpu.reg_write(UC_ARM_REG_SP, 0xdead0000)
            cpu.reg_write(UC_ARM_REG_R0, data)
            cpu.reg_write(UC_ARM_REG_R1, len(payload))
            cpu.reg_write(UC_ARM_REG_LR, code + 0xffc)
            preserved = (UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
                         UC_ARM_REG_R10, UC_ARM_REG_R11)
            for reg in preserved:
                cpu.reg_write(reg, 0xcafe0000 + reg)
            cpu.emu_start(code + self.layout['image_hash_offset'], code + 0xffc,
                          timeout=1_000_000, count=100_000)
            expected = 2166136261
            for byte in payload:
                expected = ((expected ^ byte) * 16777619) & 0xffffffff
            actual = cpu.reg_read(UC_ARM_REG_R0)
            self.assertEqual(actual, expected)
            self.assertEqual(cpu.reg_read(UC_ARM_REG_SP), 0xdead0000)
            for reg in preserved:
                self.assertEqual(cpu.reg_read(reg), 0xcafe0000 + reg)
            hashes.append(actual)
        self.assertEqual(hashes[:3], [0x811c9dc5, 0xe40c292c, 0xbf9cf968])
        self.assertNotEqual(hashes[-1], hashes[-2])


if __name__ == "__main__":
    unittest.main()
