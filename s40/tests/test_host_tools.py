# SPDX-License-Identifier: MIT
"""Synthetic table/ring inputs. No firmware or memory ownership claims."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from armv5_mmu import PhysicalMemory, TranslationError, translate
from identity_map import build_tables
from read_ramlog import CAPACITY, MAGIC, decode


class Tables(unittest.TestCase):
    def test_section_and_fcse(self):
        l1 = bytearray(16384)
        struct.pack_into("<I", l1, 0x44 * 4, 0x10800000 | (2 << 10) | (7 << 5) | 0x1e)
        mem = PhysicalMemory([(0x10000000, l1)])
        m = translate(mem, 0x10000000, 0x00401234, 0x04000000)
        self.assertEqual((m.mva, m.pa, m.domain, m.ap, m.granule),
                         (0x04401234, 0x10801234, 7, 2, 1 << 20))
        self.assertTrue(m.cacheable and m.bufferable)

    def test_large_small_fine_and_tiny(self):
        l1, l2 = bytearray(16384), bytearray(4096)
        struct.pack_into("<I", l1, 0xd80 * 4, 0x10008000 | 0x11)
        for index in range(16):
            struct.pack_into("<I", l2, index * 4, 0x10a00000 | (0xe4 << 4) | 1)
        mem = PhysicalMemory([(0x10000000, l1), (0x10008000, l2)])
        for quarter in range(4):
            m = translate(mem, 0x10000000, 0xd8000001 + quarter * 0x4000)
            self.assertEqual((m.pa, m.ap), (0x10a00001 + quarter * 0x4000, quarter))
        struct.pack_into("<I", l2, 0, 0x10b05000 | (0xe4 << 4) | 2)
        for quarter in range(4):
            m = translate(mem, 0x10000000, 0xd8000001 + quarter * 1024)
            self.assertEqual((m.pa, m.ap, m.granule), (0x10b05001 + quarter * 1024, quarter, 4096))
        struct.pack_into("<I", l2, 0, 0x10b05400 | (1 << 4) | 3)
        with self.assertRaises(TranslationError):
            translate(mem, 0x10000000, 0xd8000000)
        struct.pack_into("<I", l1, 0xd80 * 4, 0x10008000 | 3)
        m = translate(mem, 0x10000000, 0xd8000007)
        self.assertEqual((m.pa, m.ap, m.granule), (0x10b05407, 1, 1024))

    def test_missing_dump_and_fault_reject(self):
        with self.assertRaises(TranslationError):
            translate(PhysicalMemory([]), 0x10000000, 0xd8000000)
        with self.assertRaises(TranslationError):
            translate(PhysicalMemory([(0x10000000, bytes(16384))]), 0x10000000, 0xd8000000)
        with self.assertRaises(ValueError):
            PhysicalMemory([(0x1000, bytes(16)), (0x1008, bytes(16))])

    def test_identity_map_both_aliases_and_faults(self):
        for va, pa in ((0xd8400000, 0x10d01000), (0x10d02000, 0x10d01000), (0x10d01000, 0x10d01000)):
            blob = build_tables(va, pa, 0x10e00000)
            mem = PhysicalMemory([(0x10e00000, blob)])
            for alias in (va, pa):
                for offset in (0, 248, 4095):
                    m = translate(mem, 0x10e00000, alias + offset)
                    self.assertEqual((m.pa, m.domain, m.ap), (pa + offset, 0, 1))
                    self.assertFalse(m.cacheable or m.bufferable)
            with self.assertRaises(TranslationError):
                translate(mem, 0x10e00000, 0x20000000)

    def test_identity_map_alignment_overflow_overlap(self):
        for args in ((0xd8400001, 0x10d01000, 0x10e00000),
                     (0xd8400000, 0x10d01000, 0x10e00001),
                     (0xd8400000, 0x10e01000, 0x10e00000),
                     (0xd8400000, 0x10d01000, 0xffffc000),
                     (0x1000, 0x10d01000, 0x10e00000)):
            with self.assertRaises(ValueError):
                build_tables(*args)


class Ramlog(unittest.TestCase):
    def test_prefix_and_wrap(self):
        for count in (0, 7, CAPACITY, CAPACITY + 1, CAPACITY * 3 + 27):
            stream = bytes(i % 251 for i in range(count))
            ring = bytearray(CAPACITY)
            for index, value in enumerate(stream):
                ring[index % CAPACITY] = value
            page = struct.pack("<4I", MAGIC, 1, count, CAPACITY) + ring
            self.assertEqual(decode(page), stream[-CAPACITY:])

    def test_bad_header(self):
        for page in (b"", bytes(4096), struct.pack("<4I", MAGIC, 2, 0, CAPACITY) + bytes(CAPACITY)):
            with self.assertRaises(ValueError):
                decode(page)


if __name__ == "__main__":
    unittest.main()
