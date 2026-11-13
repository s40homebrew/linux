#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Install RM-443 platform support in a Linux 6.12 source tree."""
import argparse
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent


def add_after(path: Path, anchor: str, insertion: str) -> None:
    content = path.read_text()
    if insertion in content:
        return
    if content.count(anchor) != 1:
        raise ValueError(f"expected one integration anchor in {path}")
    path.write_text(content.replace(anchor, anchor + insertion))


def remove_insertion(path: Path, insertion: str) -> None:
    content = path.read_text()
    if insertion in content:
        path.write_text(content.replace(insertion, ""))


def replace_once(path: Path, anchor: str, replacement: str) -> None:
    content = path.read_text()
    if replacement in content:
        return
    if content.count(anchor) != 1:
        raise ValueError(f"expected one integration anchor in {path}")
    path.write_text(content.replace(anchor, replacement))


def early_usb_console(source: Path) -> None:
    # UDC/net core are ready at level 4. The tty core's level-5 initcall is
    # linked before USB. Register functions at 5, then the legacy gadget at
    # 5s; it remains on the gadget bus until the board's level-6 UDC probe.
    # Other architectures and loadable modules retain their upstream order.
    guard = "#if defined(CONFIG_ARCH_NOKIA_RAPS) && !defined(MODULE)"
    serial = source / "drivers/usb/gadget/function/u_serial.c"
    anchor = "module_init(userial_init);"
    replace_once(serial, anchor,
                 f"{guard}\nfs_initcall(userial_init);\n#else\n{anchor}\n#endif")
    for name, instance, function in (
            ("acm", "acm_alloc_instance", "acm_alloc_func"),
            ("ecm", "ecm_alloc_inst", "ecm_alloc")):
        path = source / f"drivers/usb/gadget/function/f_{name}.c"
        anchor = f"DECLARE_USB_FUNCTION_INIT({name}, {instance}, {function});"
        replacement = f"""{guard}
DECLARE_USB_FUNCTION({name}, {instance}, {function});
static int __init {name}mod_init(void)
{{
\treturn usb_function_register(&{name}usb_func);
}}
fs_initcall({name}mod_init);
#else
{anchor}
#endif"""
        replace_once(path, anchor, replacement)
    composite = source / "drivers/usb/gadget/legacy/cdc2.c"
    anchor = "module_usb_composite_driver(cdc_driver);"
    replace_once(composite, anchor, f"""{guard}
/* Bind as soon as the board UDC appears, before MMC and display probing. */
static int __init cdc_driver_init(void)
{{
\treturn usb_composite_probe(&cdc_driver);
}}
fs_initcall_sync(cdc_driver_init);
#else
{anchor}
#endif""")


def protect_usb_console_submit(source: Path) -> None:
    serial = source / "drivers/usb/gadget/function/u_serial.c"
    anchor = """\tspin_unlock_irq(&cons->lock);
\tif (usb_ep_queue(ep, req, GFP_ATOMIC))
\t\treq->length = 0;
\tspin_lock_irq(&cons->lock);"""
    replacement = """#if defined(CONFIG_ARCH_NOKIA_RAPS) && !defined(CONFIG_SMP)
\t/* On this single CPU, keep reset/disconnect IRQs from freeing req
\t * between dropping the console lock and submitting it to the UDC.
\t * The RAPS queue operation is atomic and never completes inline.
\t * Drop the lock itself, preserving the normal callback lock order. */
\tspin_unlock(&cons->lock);
\tif (usb_ep_queue(ep, req, GFP_ATOMIC))
\t\treq->length = 0;
\tspin_lock(&cons->lock);
#else
""" + anchor + "\n#endif"
    replace_once(serial, anchor, replacement)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    makefile = (source / "Makefile").read_text()
    if not makefile.startswith("# SPDX-License-Identifier: GPL-2.0\nVERSION = 6\nPATCHLEVEL = 12\nSUBLEVEL = 0\n"):
        parser.error("expected the pinned Linux v6.12 tree")
    overlay = HERE / "overlay"
    for path in overlay.rglob("*"):
        if path.is_file():
            target = source / path.relative_to(overlay)
            target.parent.mkdir(parents=True, exist_ok=True)
            # Preserve destination timestamps on a no-op run, otherwise Kbuild
            # recompiles every overlay object even when no source changed.
            if not target.is_file() or target.read_bytes() != path.read_bytes():
                shutil.copyfile(path, target)
    add_after(source / "arch/arm/Kconfig",
              'source "arch/arm/mach-nomadik/Kconfig"\n',
              '\nsource "arch/arm/mach-nokia-raps/Kconfig"\n')
    add_after(source / "arch/arm/Makefile",
              "machine-$(CONFIG_ARCH_NOMADIK)\t\t+= nomadik\n",
              "machine-$(CONFIG_ARCH_NOKIA_RAPS)\t\t+= nokia-raps\n")
    remove_insertion(source / "drivers/gpu/drm/tiny/Kconfig",
                     '\nsource "drivers/gpu/drm/tiny/Kconfig.rm443"\n')
    remove_insertion(source / "drivers/gpu/drm/tiny/Makefile",
                     "\nobj-$(CONFIG_DRM_RM443) += rm443.o\n")
    add_after(source / "drivers/gpu/drm/Kconfig",
              'source "drivers/gpu/drm/tiny/Kconfig"\n',
              '\nsource "drivers/gpu/drm/raps/Kconfig"\n')
    drm_makefile = source / "drivers/gpu/drm/Makefile"
    add_after(drm_makefile, "obj-y\t\t\t+= tiny/\n",
              "obj-$(CONFIG_DRM_RM443) += raps/\n")
    add_after(source / "drivers/dma/Kconfig", "if DMADEVICES\n",
              '\nsource "drivers/dma/Kconfig.raps"\n')
    add_after(source / "drivers/dma/Makefile",
              "# SPDX-License-Identifier: GPL-2.0\n",
              "\nobj-$(CONFIG_RAPS_DMA) += raps.o\n")
    early_usb_console(source)
    protect_usb_console_submit(source)
    print(f"Synchronized RM-443 platform support in {source}")


if __name__ == "__main__":
    main()
