# SPDX-License-Identifier: MIT
"""Device-tree layout validation."""
import unittest
from make_dts import generate


class LayoutTests(unittest.TestCase):
    def setUp(self):
        self.args = dict(base=0x10000000, size=0x1000000,
                         kernel_bytes=0x200000, dtb=0x10FE0000,
                         dtb_bytes=0x10000, ramlog=0x10FF0000)

    def test_synthetic_layout(self):
        dts = generate(**self.args, reserved=((0x10FD0000, 0x10000),))
        self.assertIn('compatible = "nokia,rm443"', dts)
        self.assertIn("retained@10fd0000", dts)
        self.assertEqual(dts.count("no-map;"), 0)

    def test_explicit_nomap(self):
        dts = generate(**self.args, nomap_reserved=((0x10C00000, 0x200000),))
        self.assertEqual(dts.count("no-map;"), 1)

    def test_safe_sd_profile_changes_only_card_bus_width_and_clock_limit(self):
        normal = generate(**self.args)
        self.assertIn("bus-width = <4>;", normal)
        self.assertIn("max-frequency = <12500000>;", normal)
        safe = generate(**self.args, sd_safe=True)
        self.assertEqual(safe, normal.replace("bus-width = <4>;", "bus-width = <1>;")
                         .replace("max-frequency = <12500000>;", "max-frequency = <400000>;"))

    def test_image_bss_overlaps_log(self):
        self.args["kernel_bytes"] = 0xFF0000
        with self.assertRaisesRegex(ValueError, "overlaps"):
            generate(**self.args)

    def test_page_tables_protected(self):
        self.args["ramlog"] = self.args["base"] + 0x4000
        with self.assertRaisesRegex(ValueError, "overlaps"):
            generate(**self.args)

    def test_bad_alignments(self):
        for key, delta in (("base", 0x1000), ("ramlog", 1), ("dtb", 4)):
            args = self.args.copy()
            args[key] += delta
            with self.subTest(key=key), self.assertRaises(ValueError):
                generate(**args)

    def test_reservation_conflict(self):
        with self.assertRaisesRegex(ValueError, "overlaps"):
            generate(**self.args, reserved=((0x10FF0000, 0x1000),))

    def test_out_of_bank(self):
        self.args["dtb"] += 0x1000000
        with self.assertRaisesRegex(ValueError, "outside"):
            generate(**self.args)

    def test_arm32_wrap(self):
        self.args["size"] = 0x100000000
        with self.assertRaisesRegex(ValueError, "ARM32"):
            generate(**self.args)


if __name__ == "__main__":
    unittest.main()
