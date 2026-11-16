# SPDX-License-Identifier: MIT
"""Check early USB integration and preserve normal/module registration paths."""
import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
from paths import KERNEL_ROOT as UPSTREAM
SPEC = importlib.util.spec_from_file_location("kernel_prepare", PROJECT / "kernel/prepare.py")
PREPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARE)
FILES = {
    "drivers/usb/gadget/function/u_serial.c": ("userial_init", "5"),
    "drivers/usb/gadget/function/f_acm.c": ("acmmod_init", "5"),
    "drivers/usb/gadget/function/f_ecm.c": ("ecmmod_init", "5"),
    "drivers/usb/gadget/legacy/cdc2.c": ("cdc_driver_init", "5s"),
}
GUARD = "#if defined(CONFIG_ARCH_NOKIA_RAPS) && !defined(MODULE)"


class UsbInitOrder(unittest.TestCase):
    def test_pinned_sources_idempotence_and_registration_modes(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C preprocessor unavailable")
        # Use the pinned upstream declaration macros, rather than replacements
        # that could hide a duplicated or missing initializer in a branch.
        composite = (UPSTREAM / "include/linux/usb/composite.h").read_text()
        functions = composite[composite.index("#define DECLARE_USB_FUNCTION("):
                              composite.index("/* messaging utils */")]
        driver = (UPSTREAM / "include/linux/device/driver.h").read_text()
        driver = driver[driver.index("#define module_driver("):
                        driver.index("/**\n * builtin_driver()")]
        prefix = r'''
#define __init
#define __exit
#define MODULE_ALIAS(x)
#define module_exit(x)
#define module_init(fn) INIT_MARKER(normal, fn)
#define fs_initcall(fn) INIT_MARKER(5, fn)
#define fs_initcall_sync(fn) INIT_MARKER(5s, fn)
#define module_usb_composite_driver(driver) module_driver(driver, usb_composite_probe, usb_composite_unregister)
'''
        with tempfile.TemporaryDirectory(prefix="rm443-usb-order-") as directory:
            source = Path(directory)
            for relative in FILES:
                target = source / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(UPSTREAM / relative, target)
            PREPARE.early_usb_console(source)
            before = {relative: ((source / relative).read_bytes(),
                                 (source / relative).stat().st_mtime_ns)
                      for relative in FILES}
            PREPARE.early_usb_console(source)
            for relative, (function, level) in FILES.items():
                path = source / relative
                self.assertEqual((path.read_bytes(), path.stat().st_mtime_ns), before[relative])
                text = path.read_text()
                block = text[text.index(GUARD):]
                block = block[:block.index("#endif") + len("#endif")]
                for defines, expected in ((["CONFIG_ARCH_NOKIA_RAPS"], level),
                                          ([], "normal"),
                                          (["CONFIG_ARCH_NOKIA_RAPS", "MODULE"], "normal")):
                    with self.subTest(file=relative, defines=defines):
                        result = subprocess.run(
                            [compiler, "-E", "-P", "-x", "c", *["-D" + name for name in defines], "-"],
                            input=prefix + functions + driver + block,
                            text=True, capture_output=True, check=True)
                        self.assertEqual(result.stdout.count("INIT_MARKER("), 1)
                        self.assertIn(f"INIT_MARKER({expected}, {function})", result.stdout)
            # Refuse changed upstream anchors instead of silently leaving one
            # dependency at its old, later initialization level.
            (source / next(iter(FILES))).write_text("/* changed upstream initializer */\n")
            with self.assertRaisesRegex(ValueError, "integration anchor"):
                PREPARE.early_usb_console(source)


if __name__ == "__main__":
    unittest.main()
