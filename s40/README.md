# Linux on the Nokia 6303 classic

Linux 6.12 and BusyBox, started from a Java ME app on the Nokia 6303 classic
(RM-443). It takes over S40 without flashing the phone. Reboot to return to S40.
The launcher discovers firmware services on 08.90 and 10.12. Linux uses 30 MiB
of the phone's 32 MiB RAM.

LCD, keypad, timers, watchdogs and USB serial work. USB networking and restart
are experimental. Audio, cellular, Bluetooth, camera, internal
flash and suspend are not implemented.

## Build

Requirements: Git, Docker, Bash, Python 3, and the S40 homebrew Java/native
toolchain. Clone the [shared SDK](https://github.com/s40homebrew/s40) into `main/`
beside this `linux/` checkout, then follow its
[dependency setup](https://github.com/s40homebrew/s40/blob/main/docs/building.md).
For another layout, set `S40_SDK_ROOT` to the SDK repository's absolute path.

From the Linux repository root:

```sh
bash s40/build.sh
```

This fetches pinned BusyBox and musl sources, builds the kernel and initramfs
in Docker, then packages `s40/dist/Nokia6303Linux.jar` and `.jad`.
To rebuild just the launcher:

```sh
bash s40/build_boot_probe.sh
```

`JOBS` sets parallelism; `BUILD_VOLUME` selects the Docker build cache.
`UPSTREAM_DIR` changes the source cache location. `KERNEL_SOURCE`,
`BUSYBOX_SOURCE` and `MUSL_SOURCE` select existing clones of the pinned sources.
The kernel build uses upstream v6.12 plus `s40/kernel/overlay/`; put board
changes in that overlay.

## Run

Install the JAR and matching JAD, open **Linux**, and choose **Start Linux**.
Display output defaults to PIO with 24-bit color; Options also offers 18/16-bit.
Use `lcd dma` in the shell to switch to DMA. The launcher waits up to
15 seconds for USB serial, then continues.

On a Linux computer:

```sh
sudo picocom -b 115200 --flow n --imap lfcrlf /dev/ttyACM0
```

The shell includes BusyBox, `hwinspect`, `memread`, `rmprobe`, `fbtest`,
`drmtest` and `lcd`. Use `lcd pio` / `lcd dma` to switch display transport.
Key 0 or the centre key shows the keypad menu; 8 powers off and 9 restarts
after unmounting.

USB networking uses `192.168.7.2/24`, with Telnet on port 23 and no login.
Assign the host interface `192.168.7.1/24`, then run `telnet 192.168.7.2`.

The root filesystem, `/tmp` and `/root` are in RAM. `card mount` opens a FAT
SD card read-only; `card mount rw` enables writes, and `card unmount` unmounts it.

Run `hwinspect collect /tmp/inspect` on the phone to save boot and hardware
snapshots. Copy that directory to the host, then decode it from this repository:

```sh
python3 s40/tools/decode_inspection.py /path/to/inspect
```

## Source

Paths below are relative to `s40/`:

- `boot_probe/`: Java launcher and native image loader.
- `handoff/`: ARM926 transition, relocation and device shutdown.
- `kernel/overlay/`: RAPS board support and drivers.
- `kernel/userspace/`: PID 1, shell setup, storage and diagnostic utilities.
- `kernel/prepare.py`: applies the overlay and Kbuild changes.

## Licenses

Host tools and independent userspace programs use [MIT](COPYING). The kernel
overlay is GPL-2.0-only except where a file says otherwise; the boot application
and ARM handoff are GPL-2.0-or-later. The combined Linux JAR is GPL version 2.

[Upstream sources](UPSTREAM.md) lists revisions and licenses. Full notices are
in `licenses/` and `boot_probe/licenses/`; the latter enter the JAR. Distribute
complete corresponding source and build materials with binaries, including
the pinned upstream sources and S40 runtime.
