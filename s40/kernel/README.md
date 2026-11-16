# RM-443 Linux kernel

Linux 6.12 board support for Nokia 6303 classic: RAPS interrupts, timers,
RAM and framebuffer consoles, keypad, watchdogs, CBUS, USB, and SD/MMC. The initramfs
contains PID 1 and static BusyBox ash. See the [project README](../README.md)
for installation and [hardware guide](../docs/hardware.md) for the register map.

## Drivers and userspace

| Component | Implementation |
| --- | --- |
| Interrupts | 226-source RAPS IRQ domain; inherited IRQ/FIQ routes masked before setup |
| Timekeeping | 32.768 kHz clocksource and 9.6 MHz channel-2 clockevent |
| LCD | RAPS DRM/KMS driver, DMA transfers, RGB565/XRGB8888 buffers, damage updates, fbdev and 30×20 fbcon |
| DMA | Six-channel RAPS DMAengine provider with interrupt completion |
| Keypad | RAPS GPIO matrix polling at 20 ms with two-sample debounce |
| Power | Serialized CBUS, RAPS and AVilma watchdogs, power key, poweroff/restart |
| USB | RAPS full-speed PIO UDC, Betty transceiver, CDC ACM and ECM functions |
| SD/MMC | RAPS PIO host, LP3929 power/level shifter, GPIO card detection, FAT/VFAT and ext4 |

Drivers use physical hardware registers. The launcher discovers firmware
services and memory mappings before Linux takes control. LCD power and timing,
SD pad mux, and most system clocks are inherited from S40.

PID 1 mounts proc, sysfs, devtmpfs, devpts and tmpfs, opens the input devices,
feeds `/dev/watchdog0`, and supervises USB shell/Telnet processes. Diagnostics
use `/dev/kmsg`; `printk.devkmsg=on` prevents the default userspace log rate limit
from dropping menu responses. `ttyRM0` supplies an output-only console.

`/tmp` uses tmpfs's RAM-relative default limit; `/dev/shm` has a 25% limit.
`memcheck 20` writes and verifies 20 MiB of anonymous memory. `card mount` is
read-only by default, `card mount rw` enables writes, and `card unmount` unmounts
nested storage before the FAT backing volume. Root remains the initramfs.

USB uses six data endpoints and EP0, Betty VBUS polling, reset handling and
hotplug support. DMA, isochronous transfers, high speed, host mode and suspend
are not implemented. `/sys/devices/platform/20036600.usb/status` provides full
status; `enumeration` provides the compact key-6 snapshot.

The phone configures `usb0` as `192.168.7.2/24`, starts `/bin/sh -i` on `ttyGS0`,
and binds Telnet to port 23 at that address. See the main README for host setup.

## Source layout

| Path | Purpose |
| --- | --- |
| `overlay/arch/arm/mach-nokia-raps/` | Board setup, IRQs, timers, power and secure-service bridge |
| `overlay/drivers/` | Linux subsystem drivers, including DRM, USB, MMC and input |
| `overlay/drivers/gpu/drm/raps/` | KMS integration and LCD transport |
| `overlay/drivers/dma/raps.c` | RAPS linear DMAengine provider |
| `userspace/init/` | PID 1, diagnostics and USB shell supervision |
| `userspace/display/` | Framebuffer/KMS tools and shared VT handling |
| `userspace/rootfs/` | Shell profile and storage commands |
| `userspace/build.sh` | Per-program compilation and cache dependencies |
| `userspace/root.list` | Initramfs file manifest |
| `prepare.py` | Overlay installation and Kbuild integration |

The overlay follows the kernel tree's directory structure. Build output stays
under `build/` or in the Docker volume.

## Watchdogs and shutdown

`/dev/watchdog0` represents both hardware deadlines. A successful ping reloads
AVilma first, then RAPS. AVilma uses a six-bit seconds field; zero powers off.
RAPS retains the ten-second period. Linux's watchdog core bridges that hardware
limit to the logical timeout, and PID 1 continues keepalives with `nowayout`.
The driver enforces timeout values 1–63 before writing hardware.

CBUS transfers share a raw spinlock and hardware semaphore. Acquisition and
busy waits have 100,000-read bounds, not calibrated timeouts. PMIC IRQ delivery
is masked while event latches and RTC controls are preserved. The power status
file reports raw register values and transfer errors; it does not decode battery
charge or a calendar. The separate power key produces an evdev event without
an automatic shutdown action.

Shutdown first unmounts storage while PID 1 continues feeding watchdogs.
Restart prepares the reserved firmware policy page, flushes caches and writes
the recovered reset command. [Power documentation](../docs/power.md) describes
the register sequences.

Fault tests accept `rm443.wdt-test=expire`, `bus`, or `bounds` in the command
line. They exercise missed keepalives, an injected stuck bus, or rejected timeout
values respectively. Shipped device trees contain none of these options.

## Build

From the Linux repository root, with Docker or OrbStack running:

```sh
bash s40/kernel/build.sh
```

The script exports the pinned Linux commit from this repository's Git history
into the case-sensitive Docker volume `s40-linux612-build`, applies
`overlay/`, and builds an ARMv5 uncompressed Image. `BUILD_VOLUME` selects another
cache volume. The toolchain image downloads its dependencies; the kernel build
container runs without network access.

The volume caches kernel objects, userspace binaries and initramfs inputs.
Overlay and configuration changes invalidate their dependent outputs;
kernel-only edits reuse userspace binaries. Each userspace program has its
own cache, so a display-tool edit does not rebuild PID 1.

Outputs in `build/` include:

- `Image` and `vmlinux`
- `kernel.config` and `elf-attributes.txt`
- `symbols.txt` and `image-layout.json`
- `source-commit.txt`

`JOBS` controls parallelism (default 4). `KERNEL_SOURCE` selects a clone
containing the pinned commit. `BUILDX_CONFIG` defaults to `build/buildx`.
For installations requiring Docker's legacy builder, use
`DOCKER_BUILDKIT=0 bash s40/kernel/build.sh`.

On Linux without Docker, install an ARM soft-float EABI compiler, GNU make,
binutils, flex, bison, bc, libelf/libssl headers, Python, and Git. Export the
pinned source to a separate directory, run
`python3 s40/kernel/prepare.py /absolute/kernel/source`, then use the
configuration and Image commands in `build-in-container.sh`. The userspace
must target ARMv5TE soft-float.

The build includes pinned BusyBox 1.38.0 and musl with no dynamic loader.
Their commits are in [upstream sources](../UPSTREAM.md),
`userspace/build-busybox.sh` and the package source notice.
The native JAR build has a separate cache; see the main README.

## Device tree and memory

The current launcher advertises 30 MiB at `0x10200000`, with Image entry at
`0x10208000`. Low 2 MiB is excluded because it contains DSP shared memory.
The loader reserves a runtime-positioned 64 KiB transition block and the
firmware warm-reset page. Memory layout and bounds checks are described in
[boot validation](../docs/boot_validation.md).

`make_dts.py --help` describes direct-entry inputs. Its bank base must be aligned
to 2 MiB for `ARM_PATCH_PHYS_VIRT`; Image entry is base plus `0x8000`.
`--kernel-memory-bytes` is `_end - _text` from the matching ELF, including BSS,
as reported by `build/image-layout.json`. Image file length alone is insufficient.
The first `0x8000` bytes include Linux's early page tables.

The DTB, log and additional `--reserve BASE:SIZE` ranges must fit without
intersection. Ordinary reservations retain linear mappings. Use
`--no-map-reserve BASE:SIZE` only for layouts that require removing the mapping:
small no-map holes caused ARM926 paging failures and bank truncation in earlier
tests. Compile with `dtc -I dts -O dtb -o chosen.dtb chosen.dts` and check the
result fits `--dtb-reserve`.

Build Image, not zImage. `CONFIG_PHYS_OFFSET=0` is a dormant decompressor setting;
Image computes and patches its physical offset from the entry address.

## RAM console

`earlycon=rm443ram,mmio32,<physical-page>` uses a reserved 4 KiB RAM page.
`console=tty0 console=ttyRM0` selects the LCD and RAM consoles. The normal RAM
console replaces earlycon and replays printk history.

The driver retains ARM's permanent `FIX_EARLYCON_MEM_BASE` IO mapping, restored
during `early_fixmap_shutdown()`. It accesses the log exclusively through that
uncached mapping after the bootloader's cache clean. The reserved cached linear
alias must remain unused. A second ioremap would violate ARM's alias guard.
Writes are serialized with an IRQ-saving raw spinlock.

| Offset | Little-endian field |
| --- | --- |
| 0 | Magic `0x5334304c` |
| 4 | Version `1` |
| 8 | Total bytes written, unsigned 32-bit |
| 12 | Capacity `4080` |
| 16 | Circular byte buffer |

Setup resets the header each boot. Before the buffer fills, data is a prefix;
afterward its oldest byte is at `written % 4080`. The count wraps at 4 GiB.
The capture tools read this page from DDR. The log is volatile; firmware may
overwrite it during startup.

## Checks and licensing

The [test guide](../tests/README.md) lists component checks.
`kernel/test_dts.py` checks the host DT generator.

The original init is MIT licensed. Linux nolibc headers are used under their
MIT option; preserve `userspace/NOTICE` with the embedded init. Kernel overlay
files are GPL-2.0-only.
