# SPDX-License-Identifier: MIT
"""Compile the native DT writer and inspect its actual bytes with dtc/libfdt tools."""
import ctypes
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]


def properties(blob):
    header = struct.unpack_from(">10I", blob)
    magic, total, structure, strings, reserve, version, compatible, boot_cpu, strings_size, structure_size = header
    assert magic == 0xD00DFEED and total == len(blob)
    assert (reserve, version, compatible, boot_cpu) == (40, 17, 16, 0)
    assert blob[reserve:reserve + 16] == bytes(16)
    assert structure % 4 == 0 and structure + structure_size <= strings
    assert strings + strings_size <= total
    position, stack, result = structure, [], {}
    while position < structure + structure_size:
        token, = struct.unpack_from(">I", blob, position)
        position += 4
        if token == 1:
            end = blob.index(0, position)
            stack.append(blob[position:end].decode())
            position = (end + 4) & ~3
        elif token == 2:
            assert stack
            stack.pop()
        elif token == 3:
            length, offset = struct.unpack_from(">2I", blob, position)
            position += 8
            assert offset < strings_size and position + length <= structure + structure_size
            end = blob.index(0, strings + offset)
            assert end < strings + strings_size
            name = blob[strings + offset:end].decode()
            path = "/".join(stack) or "/"
            key = (path, name)
            assert key not in result
            result[key] = blob[position:position + length]
            position += (length + 3) & ~3
        elif token == 9:
            assert not stack and position == structure + structure_size
            return result
        else:
            raise AssertionError(f"unexpected DT token {token}")
    raise AssertionError("missing FDT_END")


class NativeFdt(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("C compiler needed")
        cls.temporary = tempfile.TemporaryDirectory(prefix="rm443-fdt-test-")
        cls.directory = Path(cls.temporary.name)
        cls.library = cls.directory / "boot_fdt.so"
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        str(PROJECT / "boot_probe/native/boot_fdt.c"), "-o", str(cls.library)], check=True)
        cls.native = ctypes.CDLL(str(cls.library))
        cls.native.linux6303_fdt.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32)
        cls.native.linux6303_fdt.restype = ctypes.c_size_t
        cls.native.linux6303_fdt_at.argtypes = cls.native.linux6303_fdt.argtypes + (ctypes.c_uint32,)
        cls.native.linux6303_fdt_at.restype = ctypes.c_size_t
        cls.native.linux6303_fdt_with_reboot.argtypes = cls.native.linux6303_fdt_at.argtypes + (ctypes.c_uint32,)
        cls.native.linux6303_fdt_with_reboot.restype = ctypes.c_size_t
        cls.native.linux6303_fdt_with_secure_watchdog.argtypes = cls.native.linux6303_fdt_with_reboot.argtypes + (ctypes.c_uint32,) * 3
        cls.native.linux6303_fdt_with_secure_watchdog.restype = ctypes.c_size_t
        cls.native.linux6303_fdt_with_secure_state.argtypes = cls.native.linux6303_fdt_with_reboot.argtypes + (ctypes.c_uint32, ctypes.c_void_p)
        cls.native.linux6303_fdt_with_secure_state.restype = ctypes.c_size_t
        cls.native.linux6303_fdt_with_options.argtypes = cls.native.linux6303_fdt_with_secure_state.argtypes + (ctypes.c_uint,)
        cls.native.linux6303_fdt_with_options.restype = ctypes.c_size_t

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def emit(self, capacity=4096, base=0x10A00000, size=0x400000, transition=None, reason=None):
        memory = (ctypes.c_ubyte * (capacity + 64))(*([0xA5] * (capacity + 64)))
        if reason is not None:
            count = self.native.linux6303_fdt_with_reboot(ctypes.addressof(memory) + 32,
                capacity, base, size, transition, reason)
        elif transition is None:
            count = self.native.linux6303_fdt(ctypes.addressof(memory) + 32, capacity, base, size)
        else:
            count = self.native.linux6303_fdt_at(ctypes.addressof(memory) + 32, capacity, base, size, transition)
        self.assertEqual(bytes(memory[:32]), bytes([0xA5]) * 32)
        self.assertEqual(bytes(memory[-32:]), bytes([0xA5]) * 32)
        self.assertLessEqual(count, capacity)
        if not count:
            self.assertEqual(bytes(memory), bytes([0xA5]) * len(memory))
        return bytes(memory[32:32 + count])

    def test_firmware_reboot_pages_reserved_and_bounds_checked(self):
        for reason in (0x10201104, 0x11c2b104):
            blob = self.emit(base=0x10200000, size=0x1e00000,
                             transition=0x11000000, reason=reason)
            props = properties(blob)
            path = f"/reserved-memory/reboot-state@{reason & ~4095:x}"
            self.assertEqual(props[(path, "reg")], struct.pack(">2I", reason & ~4095, 4096))
            self.assertEqual(props[(path, "compatible")], b"nokia,rm443-reboot-state\0")
            self.assertEqual(props[(path, "nokia,reset-reason-offset")], struct.pack(">I", 0x104))
            self.assertNotIn((path, "no-map"), props)
        for reason in (0x101ff104, 0x12000104, 0x11000104, 0x1100f104, 0x10201f00, 0x10201105):
            with self.subTest(reason=hex(reason)):
                self.assertEqual(self.emit(base=0x10200000, size=0x1e00000,
                    transition=0x11000000, reason=reason), b"")

    def test_secure_watchdog_reservations(self):
        def emit(secure=0x11010000, work=0x11c06000, work_bytes=0x3000):
            storage = ctypes.create_string_buffer(4096)
            size = self.native.linux6303_fdt_with_secure_watchdog(storage, len(storage),
                0x10200000, 0x1e00000, 0x11000000, 0x11c2b104,
                secure, work, work_bytes)
            return storage.raw[:size]
        props = properties(emit())
        for path, start, size in (("secure-watchdog@11010000", 0x11010000, 0x20000),
                                   ("secure-work@11c06000", 0x11c06000, 0x3000)):
            path = "/reserved-memory/" + path
            self.assertEqual(props[(path, "reg")], struct.pack(">2I", start, size))
            self.assertNotIn((path, "no-map"), props)
        for secure, work, size in ((0x11008000, 0x11c06000, 0x3000),
                                   (0x11010000, 0x11020000, 0x3000),
                                   (0x11010000, 0x11c2b000, 0x1000),
                                   (0x11010000, 0x11fff000, 0x3000),
                                   (0, 0x11c06000, 0x3000)):
            self.assertEqual(emit(secure, work, size), b"")

    def test_secure_live_allocations_fit_dtb_and_keep_shared_pages(self):
        state = (ctypes.c_uint32 * 64)()
        state[6], state[7] = 0x11c06a00, 8192
        for i in range(4):
            state[33 + i], state[37 + i] = 0x10b66a98 + 0x700 * i, 0x4a0
        storage = ctypes.create_string_buffer(4096)
        count = self.native.linux6303_fdt_with_secure_state(storage, len(storage),
            0x10200000, 0x1e00000, 0x11000000, 0x11c2b104, 0x11010000, state)
        self.assertGreater(count, 0)
        props = properties(storage.raw[:count])
        self.assertEqual(props[("/reserved-memory/secure-work@11c06000", "reg")],
                         struct.pack(">2I", 0x11c06000, 0x3000))
        for i in range(4):
            pa, size = state[33 + i], state[37 + i]
            page = pa & ~4095
            self.assertEqual(props[(f"/reserved-memory/secure-allocation{i}@{page:x}", "reg")],
                struct.pack(">2I", page, ((pa & 4095) + size + 4095) & ~4095))

    def test_launcher_display_options_preserve_hardware_and_reservations(self):
        state = (ctypes.c_uint32 * 64)()
        state[6], state[7] = 0x11c06a00, 8192
        for i in range(4):
            state[33 + i], state[37 + i] = 0x10b66a98 + 0x700 * i, 0x4a0
        baseline = None
        for color_bits, format_name in ((0, b"rgb565"), (2, b"rgb666"), (4, b"rgb888")):
            for dma in (0, 1):
                storage = ctypes.create_string_buffer(4096)
                count = self.native.linux6303_fdt_with_options(storage, len(storage),
                    0x10200000, 0x1e00000, 0x11000000, 0x11c2b104,
                    0x11010000, state, color_bits | dma)
                self.assertGreater(count, 0)
                props = properties(storage.raw[:count])
                args = props.pop(("/chosen", "bootargs"))
                self.assertIn(b"rm443.display=" + (b"dma" if dma else b"pio") + b" ", args)
                self.assertTrue(args.endswith(b"rm443.lcd=" + format_name + b"\0"))
                if baseline is None:
                    baseline = props
                else:
                    self.assertEqual(props, baseline)
        for invalid in (6, 7, 8, 0xffffffff):
            storage = ctypes.create_string_buffer(b"\xa5" * 4096, 4096)
            self.assertEqual(self.native.linux6303_fdt_with_options(storage, len(storage),
                0x10200000, 0x1e00000, 0x11000000, 0x11c2b104,
                0x11010000, state, invalid), 0)
            self.assertEqual(storage.raw, b"\xa5" * 4096)

    def test_actual_blob_layout_and_dtc_roundtrip(self):
        blob = self.emit()
        props = properties(blob)
        self.assertEqual(props[("/", "compatible")], b"nokia,rm443\0")
        self.assertEqual(props[("/memory@10a00000", "reg")], struct.pack(">2I", 0x10A00000, 0x400000))
        self.assertEqual(props[("/reserved-memory/transition@10df0000", "reg")], struct.pack(">2I", 0x10DF0000, 0xF000))
        self.assertNotIn(("/reserved-memory/transition@10df0000", "no-map"), props)
        self.assertEqual(props[("/reserved-memory/ramlog@10dff000", "reg")], struct.pack(">2I", 0x10DFF000, 0x1000))
        self.assertNotIn(("/reserved-memory/ramlog@10dff000", "no-map"), props)
        interrupt_parent = props[("/interrupt-controller@40000000", "phandle")]
        self.assertEqual(props[("/timer@20040000", "interrupt-parent")], interrupt_parent)
        self.assertEqual(props[("/usb@20036600", "interrupt-parent")], interrupt_parent)
        self.assertEqual(props[("/display@20022200", "dmas")][:4],
                         props[("/dma-controller@20040c00", "phandle")])
        if shutil.which("dtc") is None:
            self.skipTest("dtc needed for independent DT reader")
        original = self.directory / "native.dtb"
        source = self.directory / "native.dts"
        rebuilt = self.directory / "rebuilt.dtb"
        original.write_bytes(blob)
        subprocess.run(["dtc", "-I", "dtb", "-O", "dts", "-o", str(source), str(original)], check=True)
        subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(rebuilt), str(source)], check=True)
        self.assertEqual(properties(rebuilt.read_bytes()), props)

    def test_safe_sd_profile_only_changes_card_bus_width_and_clock_limit(self):
        library = self.directory / "boot_fdt_sd_safe.so"
        subprocess.run([shutil.which("cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-shared", "-fPIC", "-DLINUX6303_SD_SAFE=1",
                        str(PROJECT / "boot_probe/native/boot_fdt.c"), "-o", str(library)], check=True)
        native = ctypes.CDLL(str(library))
        native.linux6303_fdt.argtypes = self.native.linux6303_fdt.argtypes
        native.linux6303_fdt.restype = ctypes.c_size_t
        storage = ctypes.create_string_buffer(4096)
        count = native.linux6303_fdt(storage, len(storage), 0x10A00000, 0x400000)
        self.assertGreater(count, 0)
        expected = properties(self.emit())
        expected[("/mmc@20032700", "bus-width")] = struct.pack(">I", 1)
        expected[("/mmc@20032700", "max-frequency")] = struct.pack(">I", 400000)
        self.assertEqual(properties(storage.raw[:count]), expected)

    def test_minimum_capacity(self):
        self.assertEqual(self.emit(4096), self.emit(8192))
        for capacity in (0, 1, 55, 56, 1023, 2048, 4095):
            with self.subTest(capacity=capacity):
                self.assertEqual(self.emit(capacity), b"")

    def test_diagnostic_args_fit_and_keep_other_properties(self):
        library = self.directory / "boot_fdt_diagnostic.so"
        subprocess.run([shutil.which("cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-shared", "-fPIC", "-DLINUX6303_SD_SAFE=1",
                        "-DLINUX6303_INSPECT=1", "-DLINUX6303_SDLOG=0",
                        "-DLINUX6303_USB_WAIT_MS=15000",
                        "-DLINUX6303_SD_POLL=1", "-DLINUX6303_SD_TRACE=1",
                        str(PROJECT / "boot_probe/native/boot_fdt.c"), "-o", str(library)], check=True)
        native = ctypes.CDLL(str(library))
        native.linux6303_fdt.argtypes = self.native.linux6303_fdt.argtypes
        native.linux6303_fdt.restype = ctypes.c_size_t
        storage = ctypes.create_string_buffer(4096)
        count = native.linux6303_fdt(storage, len(storage), 0x10A00000, 0x400000)
        self.assertGreater(count, 0)
        expected = properties(self.emit())
        expected[("/mmc@20032700", "bus-width")] = struct.pack(">I", 1)
        expected[("/mmc@20032700", "max-frequency")] = struct.pack(">I", 400000)
        expected[("/chosen", "bootargs")] = expected[("/chosen", "bootargs")][:-1] + (
            b" rm443.inspect=1 rm443.sdlog=0 rm443_usb.boot_wait_ms=15000"
            b" rm443_mmc.poll=1 rm443_mmc.trace=1\0")
        self.assertEqual(properties(storage.raw[:count]), expected)


    def test_variable_arena_reservations(self):
        for size in (0x600000, 0x800000):
            with self.subTest(size=size):
                props = properties(self.emit(base=0x10000000, size=size))
                self.assertEqual(props[("/memory@10000000", "reg")],
                                 struct.pack(">2I", 0x10000000, size))
                transition, ramlog = 0x10000000 + size - 0x10000, 0x10000000 + size - 0x1000
                self.assertEqual(props[(f"/reserved-memory/transition@{transition:x}", "reg")],
                                 struct.pack(">2I", transition, 0xf000))
                self.assertEqual(props[(f"/reserved-memory/ramlog@{ramlog:x}", "reg")],
                                 struct.pack(">2I", ramlog, 0x1000))

    def test_invalid_arenas_rejected_without_writes(self):
        for base, size in ((0x10A01000, 0x400000), (0x10A00000, 0x100000),
                           (0x10A00000, 0), (0xFFE00000, 0x400000)):
            with self.subTest(base=base, size=size):
                self.assertEqual(self.emit(base=base, size=size), b"")

    def test_full_ddr_with_independent_retained_block(self):
        for scratch in (0x10400000, 0x10ABC000, 0x11FF0000):
            props = properties(self.emit(base=0x10000000, size=0x2000000, transition=scratch))
            self.assertEqual(props[("/memory@10000000", "reg")], struct.pack(">2I", 0x10000000, 0x2000000))
            self.assertEqual(props[(f"/reserved-memory/transition@{scratch:x}", "reg")],
                             struct.pack(">2I", scratch, 0xf000))
            self.assertEqual(props[(f"/reserved-memory/ramlog@{scratch + 0xf000:x}", "reg")],
                             struct.pack(">2I", scratch + 0xf000, 4096))
            self.assertIn(f"earlycon=rm443ram,mmio32,0x{scratch + 0xf000:08x}".encode(),
                          props[("/chosen", "bootargs")])
            self.assertFalse(any(name == "no-map" for path, name in props))

    def test_bad_full_ddr_layouts_do_not_write(self):
        for base, size, scratch in ((0x10000000, 0x2200000, 0x11000000),
                                   (0x0fe00000, 0x2000000, 0x11000000),
                                   (0x10200000, 0x2000000, 0x11000000),
                                   (0x10000000, 0x2000000, 0x11ff4000),
                                   (0x10000000, 0x2000000, 0x0fff0000),
                                   (0x10000000, 0x2000000, 0x11001000)):
            with self.subTest(base=base, size=size, scratch=scratch):
                self.assertEqual(self.emit(base=base, size=size, transition=scratch), b"")


if __name__ == "__main__":
    unittest.main()
