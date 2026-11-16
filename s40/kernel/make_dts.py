#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate an RM-443 device tree for the supplied physical RAM bank.

Exclude memory owned by the modem and DMA before selecting the bank.
"""
import argparse
from pathlib import Path


def number(value: str) -> int:
    return int(value, 0)


def generate(base: int, size: int, kernel_bytes: int, dtb: int,
             dtb_bytes: int, ramlog: int,
             reserved: tuple[tuple[int, int], ...] = (),
             nomap_reserved: tuple[tuple[int, int], ...] = (),
             reboot_reason: int = 0, display_dma: bool = True,
             sd_safe: bool = False) -> str:
    if base < 0 or base & 0x1FFFFF:
        raise ValueError("RAM base must be nonnegative and 2 MiB aligned")
    if size < 0x400000 or size & 0xFFF or base + size > 0x100000000:
        raise ValueError("RAM size must be page aligned, >= 4 MiB and fit ARM32")
    if kernel_bytes <= 0 or dtb_bytes <= 0:
        raise ValueError("kernel memory footprint and DTB reservation must be positive")
    if dtb & 7 or ramlog & 0xFFF or ramlog == 0:
        raise ValueError("DTB needs 8-byte alignment; log needs nonzero page alignment")
    ranges = [
        ("kernel and early page tables", base, base + 0x8000 + kernel_bytes),
        ("DTB", dtb, dtb + dtb_bytes),
        ("RAM log", ramlog, ramlog + 0x1000),
    ]
    extra_nodes = []
    if reboot_reason:
        if reboot_reason & 3 or (reboot_reason & 4095) > 4096 - 0x128:
            raise ValueError("reset reason/token must fit one aligned RAM page")
        page = reboot_reason & ~4095
        ranges.append(("firmware reboot state", page, page + 4096))
        extra_nodes.append(f'''        reboot-state@{page:x} {{
            compatible = "nokia,rm443-reboot-state";
            reg = <0x{page:08x} 0x1000>;
            nokia,reset-reason-offset = <0x{reboot_reason & 4095:x}>;
        }};''')
    all_reserved = [(item, False) for item in reserved] + [(item, True) for item in nomap_reserved]
    for index, ((address, length), nomap) in enumerate(all_reserved):
        if address < 0 or address & 0xFFF or length <= 0 or length & 0xFFF:
            raise ValueError("extra reservations require page-aligned address and positive size")
        ranges.append((f"reservation {index}", address, address + length))
        mapping = "            no-map;\n" if nomap else ""
        extra_nodes.append(f'''        retained@{address:x} {{
            reg = <0x{address:08x} 0x{length:08x}>;
{mapping}\
        }};''')
    for name, start, end in ranges:
        if start < base or end > base + size:
            raise ValueError(f"{name} is outside supplied RAM")
    for index, (name, start, end) in enumerate(ranges):
        for other, low, high in ranges[index + 1:]:
            if start < high and low < end:
                raise ValueError(f"{name} overlaps {other}")
    display_dma_properties = '        dmas = <&dma 9>;\n        dma-names = "tx";' if display_dma else ''
    dma_node = '''    dma: dma-controller@20040c00 {
        compatible = "nokia,raps-dma";
        reg = <0x20040c00 0x1800>, <0x20040804 0x1c>, <0x20040780 0xc>;
        #dma-cells = <1>;
        interrupt-parent = <&intc>;
        interrupts = <0 1 2 3 4 5>;
    };''' if display_dma else ''
    return f'''/dts-v1/;
/* SPDX-License-Identifier: GPL-2.0-only */
/ {{
    compatible = "nokia,rm443";
    model = "Nokia 6303 classic";
    #address-cells = <1>;
    #size-cells = <1>;
    chosen {{
        bootargs = "earlycon=rm443ram,mmio32,0x{ramlog:08x} console=tty0 consoleblank=0 fbcon=font:VGA8x16 console=ttyRM0 console=ttyGS0 loglevel=7 printk.devkmsg=on panic=0 g_cdc.dev_addr=02:00:00:44:43:02 g_cdc.host_addr=02:00:00:44:43:01 g_cdc.qmult=1 g_cdc.iManufacturer=Linux g_cdc.iProduct=\\"Nokia 6303\\" coherent_pool=16K";
    }};
    cpus {{
        #address-cells = <1>;
        #size-cells = <0>;
        cpu@0 {{
            device_type = "cpu";
            compatible = "arm,arm926ej-s";
            reg = <0>;
        }};
    }};
    memory@{base:x} {{
        device_type = "memory";
        reg = <0x{base:08x} 0x{size:08x}>;
    }};
    intc: interrupt-controller@40000000 {{
        compatible = "nokia,rm443-intc";
        reg = <0x40000000 0x1000>;
        interrupt-controller;
        #interrupt-cells = <1>;
        #address-cells = <0>;
    }};
    timer@20040000 {{
        compatible = "nokia,rm443-timer";
        reg = <0x20040000 0x498>;
        interrupt-parent = <&intc>;
        interrupts = <36>;
        clock-frequency = <9600000>;
    }};
    display@20022200 {{
        compatible = "nokia,rm443-lcd";
        reg = <0x20022200 0x20>;
{display_dma_properties}
    }};
{dma_node}
    keypad@20031cc0 {{
        compatible = "nokia,rm443-keypad";
        reg = <0x20031cc0 0xe4>;
    }};
    power@20030700 {{
        compatible = "nokia,rm443-power";
        reg = <0x20030700 0x40>, <0x20040204 4>, <0x20040500 0xc>,
              <0x20032c18 4>, <0x20032d14 4>;
        reg-names = "cbus", "semaphore", "watchdog", "reset-status", "reset-control";
    }};
    usb@20036600 {{
        compatible = "nokia,rm443-usb";
        reg = <0x20036600 0x100>, <0x20036700 0x100>, <0x20032c80 0x104>,
              <0x2004020c 4>, <0x20031c68 4>;
        reg-names = "udc", "otg", "clocks", "semaphore", "pinmux";
        interrupt-parent = <&intc>;
        interrupts = <153 154>;
        interrupt-names = "general", "endpoint";
        dr_mode = "peripheral";
        maximum-speed = "full-speed";
    }};
    mmc@20032700 {{
        compatible = "nokia,rm443-mmc";
        reg = <0x20032700 0x80>;
        interrupt-parent = <&intc>;
        interrupts = <139>;
        bus-width = <{1 if sd_safe else 4}>;
        clock-frequency = <96000000>;
        max-frequency = <{400000 if sd_safe else 12500000}>;
    }};
    reserved-memory {{
        #address-cells = <1>;
        #size-cells = <1>;
        ranges;
        ramlog@{ramlog:x} {{
            reg = <0x{ramlog:08x} 0x1000>;
        }};
{chr(10).join(extra_nodes)}
    }};
}};
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ram-base", type=number, required=True)
    parser.add_argument("--ram-size", type=number, required=True)
    parser.add_argument("--kernel-memory-bytes", type=number, required=True,
                        help="_end - _text from matching vmlinux, including BSS")
    parser.add_argument("--dtb-address", type=number, required=True)
    parser.add_argument("--dtb-reserve", type=number, default=0x10000)
    parser.add_argument("--ramlog", type=number, required=True)
    parser.add_argument("--reboot-reason", type=number, default=0,
                        help="explicitly discovered firmware warm-reset reason address")
    parser.add_argument("--reserve", action="append", default=[], metavar="BASE:SIZE",
                        help="reserve a page-aligned range while retaining its linear map")
    parser.add_argument("--no-map-reserve", action="append", default=[], metavar="BASE:SIZE",
                        help="explicitly exclude a range from the linear map; layout needs kernel validation")
    parser.add_argument("--display-dma", action=argparse.BooleanOptionalAction, default=True,
                        help="include the DMA controller and LCD link; rendering still defaults to PIO")
    parser.add_argument("--sd-safe", action="store_true",
                        help="diagnostic SD profile: one data line and at most 400 kHz")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        def parse_ranges(values):
            result = tuple(tuple(number(part) for part in value.split(":")) for value in values)
            if any(len(pair) != 2 for pair in result):
                raise ValueError("reservations must be BASE:SIZE")
            return result
        reserved = parse_ranges(args.reserve)
        nomap_reserved = parse_ranges(args.no_map_reserve)
        result = generate(args.ram_base, args.ram_size, args.kernel_memory_bytes,
                          args.dtb_address, args.dtb_reserve, args.ramlog, reserved,
                          nomap_reserved, args.reboot_reason, args.display_dma, args.sd_safe)
    except ValueError as error:
        parser.error(str(error))
    args.output.write_text(result)
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
