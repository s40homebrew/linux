# SD storage on the Nokia 6303 classic

The RAPS SD controller exposes cards through Linux MMC block devices.
Linux supports MBR partitions, FAT/VFAT, ext4 and loop devices. The `card`
command manages mounts; `sdlog` saves kernel messages to the FAT volume.

## How the card fits into boot

The phone boots S40 from its internal flash. You launch the Linux JAR,
which contains Linux and its small BusyBox initramfs. Linux takes over the CPU
and RAM, reinitializes the card controller, discovers the card and exposes an
ordinary Linux block device. SD storage is therefore optional for getting the
current shell running; it supplies persistent files and room for a larger
environment afterward.

This does not replace the phone's bootloader, change its internal flash, or make
it boot directly from SD when powered on. A later initramfs can mount a Linux
root filesystem on the card and execute its `/sbin/init` with `switch_root`.
That transition needs an installed ARMv5 soft-float root filesystem and an
explicit boot policy; adding the SD driver alone does not perform it.

You do not need a separate Linux card. There are three useful layouts:

| Layout | S40 sees | Linux sees | Use |
| --- | --- | --- | --- |
| Existing FAT card, ordinary files | Normal phone files, JAR and Linux data files | The same FAT volume | Simplest first test; no repartitioning |
| Existing FAT card containing `Linux/linux-data.ext4` | One ordinary large file | An ext4 filesystem through a loop device | Unix permissions, symlinks and Linux software without changing the card layout |
| MBR, first partition FAT16/FAT32, second partition ext4 | Expected to use the first FAT partition | Both partitions | Cleaner dedicated Linux storage once S40 coexistence is tested |

An ext4 image is a disk filesystem stored inside one file. Its contents stay on
SD; mounting it does not load the complete image into the phone's 32 MiB RAM.
FAT does not preserve Unix ownership, executable mode bits or symbolic links, so
it is suitable for shared files but awkward as a normal Linux root. The ext4
image supplies those semantics. A FAT32-hosted image must remain below FAT's 4
GiB per-file limit, and also fit the available card space. Both filesystems must
be flushed and unmounted in the correct order: ext4 image first, outer FAT
volume second.

A separate card is convenient for experiments, but it can use the same
FAT-plus-image layout. If a card is entirely ext4, S40 will not be able to read
the JAR from it; the JAR would need to be installed in phone memory. Keeping a
FAT first partition avoids that dependency. Do not accept a phone prompt to
format a Linux partitioned card unless you intend to erase its layout.

The Nokia-authored [6303 classic user guide, page
7](https://e-gain.s3.amazonaws.com/external/content/Devices/Nokia%206303/Nokia_6303_UG_en.pdf)
specifies microSD support up to **4 GB**. Larger-card behavior is not
established by this port. SDHC block addressing is a card protocol distinction;
FAT versus ext4 is a separate choice above the block driver.

## Firmware partition handling

In 10.12, `F0187DCC` reads physical sector zero. It first checks for a
FAT-style boot sector. Otherwise it copies the 66 bytes starting at byte 446,
checks the `55 aa` signature, validates the **first** partition entry's type
through `F0184DB6`, and reads that entry's start LBA and length. Accepted type
bytes are `01`, `04`, `06`, `0e`, `0b`, `0c`, and `55`. There is no scan of the
other three MBR entries in this routine. `F0187EA8` then mounts the selected FAT
volume; the formatter at `F0187770` explicitly selects FAT12, FAT16 or FAT32.
The parser supports using the first FAT partition. Preservation of a second ext4
partition across S40 Gallery, formatting, and USB modes still needs testing.

A FAT volume may occupy the whole card (`/dev/mmcblk0`) or a partition such
as `/dev/mmcblk0p1`. Use `card status` to inspect the detected layout.

## Hardware contract

The firmware virtual addresses below are evidence locations. The Linux driver
uses physical device registers and standard SD commands, not calls into S40.

| Function | Recovered contract | Evidence |
| --- | --- | --- |
| Host controller | PA `0x20032700`, aperture `0x80`; 32-bit CPU register slots, 16-bit FIFO data | Command writer `F02E75C6`, DMA descriptor `F110DE28` |
| Command/data interrupt | RAPS IRQ 139 | `F02E7640` |
| Presence | GPIO46, PA `0x20031d88` bit14, high means inserted | `F018CFB6`, `F018CDD4`, table `F110D4FC` |
| Detect interrupt | IRQ204 | GPIO table `F110E220`; initial Linux polling can avoid a new GPIO IRQ driver |
| DMA | RX request10, TX request11, peripheral width16 | `F02E7916`, `F110DE28`; PIO does not need these channels |
| Controller reset | `SYSC+0x64 = 2`, enable `CON[11]`, select source3/divider316, then poll `SYSS+0x68 == 1` | `F02E8678`; same order in both inspected firmware versions |
| Divider | `CON+0x0c` bits9:0; zero stops the card clock | `F02E7362`, corroborated by TI controller manual |
| Clock selector | PA `0x20032c10` bits11:10, preserve other fields | `F02E7362` |
| Functional clock gate | Write bit5 to PA `0x20032cd4` to set; `0x20032cd8` to clear | Resource group3 at `F110DC34`, entry5 at `F110DA9C`, routines `F037BAD8/F037BAD0` |
| Shared PLL2 enable | Write bit15 to PA `0x20032c88`, poll PA `0x20032d80` bit0 | Resource group3 entry3, `F037BAA2` → `F037BC60` |

The command/argument/status/data register layout corresponds to the [TI OMAP5912
MMC reference guide, SPRU765A](https://www.ti.com/lit/ug/spru765a/spru765a.pdf).
Relevant generic controller semantics include the ten-bit divider, W1C status,
the power-enable bit in `CON[11]`, initialization stream and FIFO thresholds.
RM-443 firmware corroborates these controller fields. The physical controller revision remains unidentified.

Firmware reset waits at least 200 ms after clearing `CON[11]`, writes the reset
and enable, then allows 20 ms settling (`F02E7338`, `F02E8678`, `F018CDBC`). It
initially chooses selector3/divider316. `F02E78D4` then chooses
selector1/divider241 for card identification, and `F018C09C` uses
selector1/divider5 after RCA assignment. The same function computes card data
timeouts with a `19200000` clock constant after selecting divider5. Its
divider19 path instead uses `5052631`, matching `96000000 / 19`. These two
independent timing calculations establish the firmware's **96 MHz nominal
parent** for selector1. That gives approximately 398 kHz for identification and
19.2 MHz with divider5. The recovered shared PLL2 frequency is 192 MHz, so the
parent being PLL2 divided by two remains a routing inference. No physical clock
measurement has been made, and selector3's rate is unresolved.

## Slot power and pins

The RM-443 service schematic identifies socket X3200 and an **LP3929 N3200**
between RAPS and the card.
Its VMMC output powers the card. The [TI LP3929
datasheet](https://media.digikey.com/pdf/Data%20Sheets/Texas%20Instruments%20PDFs/LP3929.pdf)
identifies a 2.85 V LDO and level translation from the 1.8 V host interface; EN
low disables both.

| Signal | MMC_CMT net | RAPS GPIO/pad |
| --- | --- | --- |
| DAT0 / DAT1 / DAT2 / DAT3 | 0 / 1 / 2 / 3 | 8 / 10 / 14 / 15 |
| CLK / CMD | 4 / 5 | 13 / 9 |
| DAT0 direction / DAT1–3 direction | 6 / 7 | 11 / 3 |
| LP3929 enable | 10 | 7 |
| CMD direction / returned clock | 11 / 13 | 12 / 2 |
| Card detection | 14 | 46 |

TI's controller `CON[11]` drives an MMC_POW output when that function is
available on a device pin, and the S40 driver uses that bit for the observed
off/on sequence. Routing it to the LP3929 enable through the inherited pinmux is
consistent with the schematic and firmware. The exact reset-state pinmux and
physical EN transition remain unresolved. The takeover path inherits the mux
established by S40; a cold bootloader would need to program it explicitly.

Physical testing confirms card enumeration, partition discovery, FAT mounting
and directory reads. Source3 reset timed out on the tested handset; the driver's
fallback to source1/divider241 completed reset. File contents, writes, ext4,
card removal and power cycling remain unverified on hardware.

Shared clocks serve other devices too. Firmware uses hardware semaphore 2
around PLL changes; acquisition reads have side effects.

## Boot logging

The launcher build selects a 1-bit SD bus capped at 400 kHz through
`LINUX6303_SD_SAFE=1` in `boot_probe/build-config.sh`. Use `--sd-safe` with
`kernel/make_dts.py` to generate the same profile. The driver services TX
FIFO requests during command completion and includes wire time in its deadlines.

The current build sets `LINUX6303_SDLOG=0`, disabling automatic logging and
keeping startup display transport on PIO. When logging is enabled in a build,
PID 1 starts `sdlog` after watchdog startup. It mounts the FAT volume writable
and creates a new
`/Linux/logs/boot-NNNNNN.kmsg` file. Raw `/dev/kmsg` records preserve sequence
numbers and kernel timestamps. The initial backlog and the pre-display
checkpoint must be flushed with `fsync` before DMA can be selected. Failed
startup logging leaves the display on PIO. Storage operations run in a child;
PID 1 continues feeding watchdogs during its bounded startup wait.

Logs are limited to 1 MiB per boot, 16 MiB retained total and 128 files. Old logs
are never overwritten or deleted automatically. `card unmount` stops the
logger and closes its file before unmounting. A later logger failure is reported
but does not automatically change an already-running display transport.

Logging starts after MMC and filesystem initialization, then drains the
retained kernel-message backlog. Messages produced after storage or kernel
execution stops cannot be flushed to the card.

## Mounting storage

`card mount` defaults to read-only. When enabled, logging requests a writable FAT mount;
the root filesystem remains the embedded initramfs.

In the BusyBox USB shell:

```sh
card status
card mount
ls /mnt/card
card unmount
```

`card mount` selects the whole FAT device or first FAT partition as appropriate
and mounts it at `/mnt/card` read-only. Use `card mount rw` to request writes;
unmount first when changing modes. `card unmount` first unmounts `/mnt/linux` if
present, then `/mnt/card`, and flushes pending writes. The PID1 power-off and
restart actions also require storage unmounts to succeed before proceeding,
while watchdog feeding continues.

For a new ext4 image, run on the development computer:

```sh
python3 s40/tools/make_linux_volume.py linux-data.ext4 --size-mib 64
```

This creates only the named **new regular file** using e2fsprogs `mke2fs`; it
refuses an existing path. Copy the file onto the normal card in a `Linux`
directory. After booting Linux, mount the card writable and then the image:

```sh
card mount rw
mount -t ext4 -o loop /mnt/card/Linux/linux-data.ext4 /mnt/linux
ls /mnt/linux
card unmount
```

Neither this helper nor mounting an image installs a distribution or changes
`/` away from the embedded initramfs. Ordinary files
copied to `/mnt/linux` persist; `/tmp` and the current root environment remain
RAM-backed.

Remaining physical checks cover clock rate, inherited pinmux, card power
cycling, command/data timing, and removal during active writes. DMA and
higher-speed modes are not implemented. Booting an SD root filesystem requires
an installed ARMv5 system and an initramfs switch_root policy.
