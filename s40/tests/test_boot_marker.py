# SPDX-License-Identifier: MIT
"""Execute the real stackless marker against a counted LCD-transfer model."""
import unittest
from support import handoff

from unicorn import UC_ARCH_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc
from unicorn.arm_const import (
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_R0,
    UC_ARM_REG_R8,
    UC_ARM_REG_R9,
    UC_ARM_REG_R10,
    UC_ARM_REG_R11,
    UC_ARM_REG_SP,
)

LCD = 0x20022200
GPIO = 0x20031c00


class BootMarker(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.blob, cls.layout = handoff()

    def execute(self, code, stuck_after=None):
        cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        cpu.mem_map(0x08000000, 4096)
        cpu.mem_write(0x08000000, self.blob)
        cpu.mem_map(LCD & ~4095, 4096)
        cpu.mem_map(GPIO & ~4095, 4096)
        mode = 0x23500000
        cpu.mem_write(LCD + 4, mode.to_bytes(4, 'little'))
        remaining, control, busy, writes = 0, 0, 0, 0
        command = None
        payloads = {}
        reset_sequence = []
        pin_writes = []
        lcd_started = False

        def read(uc, access, address, size, value, user):
            nonlocal busy
            if address != LCD + 8:
                return
            stuck = stuck_after is not None and writes >= stuck_after
            uc.mem_write(address, (0 if busy or remaining or stuck else 8).to_bytes(4, 'little'))
            if busy:
                busy -= 1

        def write(uc, access, address, size, value, user):
            nonlocal remaining, control, busy, writes, command, lcd_started
            if GPIO <= address < GPIO + 0x200:
                pin_writes.append((address - GPIO, value))
                return
            self.assertIn(address, (LCD + 4, LCD + 8, LCD + 16))
            if address == LCD + 8:
                if not lcd_started:
                    self.assertEqual(value & 1, 0)
                    lcd_started = True
                    return
                reset_sequence.append(value)
                return
            if address == LCD + 4:
                self.assertEqual(pin_writes, [(0x28, 0xffff), (0x24, 0x5555),
                                              (0xe8, 0xff0000)])
                self.assertEqual(remaining, 0, 'previous transfer truncated')
                self.assertEqual(busy, 0, 'previous transfer still in flight')
                self.assertEqual(reset_sequence, [2, 1], 'missing FIFO reset/enable')
                reset_sequence.clear()
                self.assertEqual(value & 0x3ff00000, mode, 'panel mode lost')
                control = value
                remaining = (value & 0xffff) + 1
                self.assertLessEqual(remaining, 8, 'oversized physical PIO transfer')
                return
            self.assertGreater(remaining, 0)
            count = min(size, remaining)  # controller honors its programmed count
            remaining -= count
            writes += 1
            if control & 0x40000:
                self.assertIsNotNone(command)
                payloads[command].extend(value.to_bytes(size, 'little')[:count])
            else:
                self.assertEqual(count, 1)
                command = value
                self.assertNotIn(command, payloads)
                payloads[command] = bytearray()
            if not remaining:
                busy = 3

        cpu.hook_add(UC_HOOK_MEM_READ, read)
        cpu.hook_add(UC_HOOK_MEM_WRITE, write)
        saved = (UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11)
        for reg in saved:
            cpu.reg_write(reg, 0xcafe0000 + reg)
        cpu.reg_write(UC_ARM_REG_R0, code)
        cpu.reg_write(UC_ARM_REG_SP, 0xdead0000)  # no mapped stack allowed
        cpu.reg_write(UC_ARM_REG_LR, 0x08000ffc)
        cpu.emu_start(0x08000000 + self.layout['marker_offset'], 0x08000ffc,
                      timeout=3_000_000, count=2_000_000)
        self.assertEqual(cpu.reg_read(UC_ARM_REG_PC), 0x08000ffc)
        self.assertEqual(cpu.reg_read(UC_ARM_REG_SP), 0xdead0000)
        self.assertEqual(cpu.reg_read(UC_ARM_REG_R0), 0xffffffff if stuck_after is not None else 0)
        for reg in saved:
            self.assertEqual(cpu.reg_read(reg), 0xcafe0000 + reg)
        return payloads, writes

    def test_counted_transfers_preserve_panel_mode_and_render_identifiable_codes(self):
        # Decode full DCS pixel payload, rather than inspecting instructions.
        masks = (0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07,
                 0x7f, 0x6f, 0x77, 0x7c, 0x39, 0x5e, 0x79, 0x71)
        segments = ((6, 2, 26, 6), (26, 6, 30, 18), (26, 22, 30, 34),
                    (6, 34, 26, 38), (2, 22, 6, 34), (2, 6, 6, 18),
                    (6, 18, 26, 22))
        for code in (0x00, 0x34, 0x40, 0x41, 0x42, 0x43, 0x4f, 0x50, 0x60, 0xff):
            with self.subTest(code=hex(code)):
                data, _ = self.execute(code)
                self.assertEqual(data[0x3a], b'\x55')
                self.assertEqual(data[0x2a], b'\0\0\0\x3f')
                self.assertEqual(data[0x2b], b'\0\0\0\x27')
                expected = bytearray(64 * 40 * 2)
                for digit, nibble in enumerate((code >> 4, code & 15)):
                    for segment, (x0, y0, x1, y1) in enumerate(segments):
                        if masks[nibble] & (1 << segment):
                            for y in range(y0, y1):
                                for x in range(x0 + digit * 32, x1 + digit * 32):
                                    offset = (y * 64 + x) * 2
                                    expected[offset:offset + 2] = b'\xff\xff'
                self.assertEqual(data[0x2c], expected)

    def test_missing_or_stalled_display_returns_without_stack_or_unbounded_wait(self):
        for after in (0, 1, 7, 100):
            with self.subTest(after=after):
                _, writes = self.execute(0x43, stuck_after=after)
                self.assertEqual(writes, after)


if __name__ == '__main__':
    unittest.main()
