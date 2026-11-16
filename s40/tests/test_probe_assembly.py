# SPDX-License-Identifier: MIT
"""Compile and inspect the real ARM probe code; never execute an ARM image."""
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MACH = ROOT / "kernel/overlay/arch/arm/mach-nokia-raps"
from paths import llvm_tool


class Elf32:
    """Only the ELF32 tables needed to check linked ARM load/fixup addresses."""
    def __init__(self, path):
        self.data = path.read_bytes()
        assert self.data[:6] == b"\x7fELF\x01\x01"
        shoff = struct.unpack_from("<I", self.data, 32)[0]
        entsize, count, strings = struct.unpack_from("<HHH", self.data, 46)
        self.headers = [struct.unpack_from("<10I", self.data, shoff + i * entsize)
                        for i in range(count)]
        names = self.section_data(self.headers[strings])
        self.sections = {self.string(names, h[0]): h for h in self.headers}
        self.symbols = {}
        for header in self.headers:
            if header[1] != 2:  # SHT_SYMTAB
                continue
            names = self.section_data(self.headers[header[6]])
            symbols = self.section_data(header)
            for offset in range(0, len(symbols), header[9]):
                name, value, size, info, other, section = struct.unpack_from(
                    "<IIIBBH", symbols, offset)
                self.symbols[self.string(names, name)] = (value, size, section)

    @staticmethod
    def string(data, offset):
        return data[offset:data.index(b"\0", offset)].decode()

    def section_data(self, header):
        return self.data[header[4]:header[4] + header[5]]

    def word_at(self, address):
        for header in self.headers:
            if header[1] == 1 and header[3] <= address < header[3] + header[5]:
                return struct.unpack_from("<I", self.data,
                                          header[4] + address - header[3])[0]
        raise AssertionError(f"No file-backed section for {address:#x}")


class ProbeAssembly(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.clang, cls.readelf, cls.linker = llvm_tool("clang"), llvm_tool("llvm-readelf"), llvm_tool("ld.lld")
        if not all((cls.clang, cls.readelf, cls.linker)):
            raise unittest.SkipTest("LLVM clang, llvm-readelf and ld.lld are required")

    def compile(self, source, destination, *flags):
        subprocess.run([self.clang, "--target=arm-none-eabi", "-march=armv5te",
                        "-ffreestanding", "-D__ASSEMBLY__", *flags,
                        "-c", str(source), "-o", str(destination)], check=True,
                       capture_output=True, text=True)

    def test_copied_bridge_is_self_contained_and_fits_its_page(self):
        with tempfile.TemporaryDirectory(prefix="rm443-probe-asm-") as directory:
            output = Path(directory) / "bridge.o"
            self.compile(MACH / "rm443_secure_wdt_call.S", output)
            relocations = subprocess.check_output([self.readelf, "-r", str(output)], text=True)
            self.assertIn("There are no relocations", relocations)
            image = Elf32(output)
            start = image.symbols["rm443_swd_bridge_start"][0]
            end = image.symbols["rm443_swd_bridge_end"][0]
            callback = image.symbols["rm443_swd_callback"][0]
            self.assertEqual(start & 31, 0)
            self.assertLess(start, callback)
            self.assertLess(callback, end)
            self.assertLessEqual(end - start, 4096)

    def test_load_widths_and_exact_fault_continuations(self):
        with tempfile.TemporaryDirectory(prefix="rm443-probe-load-") as directory:
            directory = Path(directory)
            (directory / "linux").mkdir()
            (directory / "asm").mkdir()
            # Only kernel declaration macros are supplied; the instructions
            # and exception table come unchanged from the production source.
            (directory / "linux/linkage.h").write_text(
                "#define ENTRY(x) .globl x; .type x, %function; x:\n"
                "#define ENDPROC(x) .size x, .-x\n")
            (directory / "asm/errno.h").write_text("#define EFAULT 14\n")
            obj, linked = directory / "load.o", directory / "load.elf"
            self.compile(MACH / "rm443_probe_load.S", obj, "-I", str(directory))
            subprocess.run([self.linker, "-Ttext=0x10000", "-e", "rm443_probe_load",
                            str(obj), "-o", str(linked)], check=True, capture_output=True)
            image = Elf32(linked)
            expected = {8: 0xe5d03000, 16: 0xe1d030b0, 32: 0xe5903000}
            sites = []
            for width, opcode in expected.items():
                address = image.symbols[f"rm443_probe_load{width}"][0]
                self.assertEqual(image.word_at(address), opcode)
                sites.append(address)
            table = image.section_data(image.sections["__ex_table"])
            self.assertEqual(len(table), 3 * 8)
            entries = list(struct.iter_unpack("<II", table))
            self.assertEqual([entry[0] for entry in entries], sites)
            self.assertEqual(len({entry[1] for entry in entries}), 1)
            fixup = entries[0][1]
            self.assertEqual(image.word_at(fixup), 0xe3e0000d)  # mvn r0,#13 => -EFAULT
            self.assertEqual(image.word_at(fixup + 4), 0xe12fff1e)  # bx lr

    def test_state_offsets_and_private_page_bounds_compile_for_arm(self):
        fields = {"mode": "MODE", "entry": "ENTRY", "regs": "REGS", "stack": "STACK",
                  "descriptor": "DESC", "result": "RESULT", "cpsr": "CPSR",
                  "count": "COUNT", "overflow": "OVERFLOW"}
        source = '#include <stddef.h>\n#include "rm443_secure_wdt_abi.h"\n'
        for field, macro in fields.items():
            source += (f'_Static_assert(offsetof(struct rm443_swd_probe_state, {field}) + '
                       f'SWD_PROBE_MODE == SWD_PROBE_{macro}, "{field}");\n')
        source += '''
_Static_assert(sizeof(struct rm443_swd_state) == 0xac, "retained capture ABI");
_Static_assert(SWD_LINUX_FCSE >= RM443_SWD_RESPONSE_OFFSET + 8, "FCSE overlaps response");
_Static_assert(SWD_LINUX_CONTEXT >= SWD_LINUX_FCSE + 4, "context overlaps FCSE");
_Static_assert(SWD_LINUX_CONTEXT + 4 <= SWD_PROBE_MODE, "context overlaps probe");
_Static_assert(sizeof(struct rm443_swd_state) <= RM443_SWD_REQUEST_OFFSET, "watchdog request overlap");
_Static_assert(SWD_PROBE_MODE >= RM443_SWD_RESPONSE_OFFSET + 8, "watchdog response overlap");
_Static_assert(SWD_PROBE_MODE + sizeof(struct rm443_swd_probe_state) <= SWD_PROBE_RECORDS, "record overlap");
_Static_assert(SWD_PROBE_RECORDS + SWD_PROBE_RECORD_MAX * SWD_PROBE_RECORD_BYTES <= RM443_SWD_FFC_MAP_OFFSET, "mapping overlap");
_Static_assert(RM443_SWD_STACK_TOP <= RM443_SWD_POOL_OFFSET, "pool overlap");
'''
        with tempfile.TemporaryDirectory(prefix="rm443-probe-layout-") as directory:
            path = Path(directory) / "layout.c"
            path.write_text(source)
            subprocess.run([self.clang, "--target=arm-none-eabi", "-march=armv5te",
                            "-ffreestanding", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(MACH), "-c", str(path), "-o", str(path.with_suffix(".o"))],
                           check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
