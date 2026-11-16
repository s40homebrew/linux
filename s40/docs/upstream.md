# Build sources and design reference

[Pinned revisions and licenses](../UPSTREAM.md) are listed once in the project
source notice. The build exports the pinned Linux commit from this checkout and
fetches BusyBox and musl, or uses `KERNEL_SOURCE`, `BUSYBOX_SOURCE` and `MUSL_SOURCE`.
The launcher also uses the [shared S40 SDK](https://github.com/s40homebrew/s40).

The [Nokia E6 Linux project](https://github.com/fluffleswaffles/rapuyama-linux)
provided a reference for taking over a running phone OS. Its board code is not
part of this port: the E6 uses ARM1176/RapuYama, Symbian and a VideoCore display;
the 6303 uses ARM926/RAPS, an S40 bootstrap and the RAPS LCD interface.
ARM11 cache operations and ARMv6 trampoline instructions do not work on ARM926.

The RM-443 loader follows the [ARM Linux boot protocol](https://docs.kernel.org/arch/arm/booting.html):
load an uncompressed Image, reserve transition storage, prepare a device tree
and enter with the required CPU state. Its own handoff translates live S40
mappings and stops known device writers before reclaiming firmware RAM.
Hardware drivers use physical registers; firmware service discovery is confined
to the launcher and retained ROM services.
