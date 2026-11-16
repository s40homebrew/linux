# Usage and hardware status

Commands below run in the Linux repository unless a phone shell or host
terminal is specified. See [building](../README.md#build) for dependencies.
Current builds retain PIO while automatic SD logging is disabled; use `lcd dma`
to select DMA explicitly. Physical observations do not establish sustained
handset stability; see [boot validation](boot_validation.md).

## Status

| Feature | Status |
| --- | --- |
| Boot, keypad, timers and watchdog service | Working on hardware; sustained stability remains unconfirmed |
| Display | DRM/KMS, framebuffer, PIO and full-frame DMA; 24-bit output corrects the observed color tint |
| USB serial | Interactive shell confirmed on hardware |
| USB Ethernet | Enumerates on hardware; IP traffic and Telnet tested in the emulator |
| SD storage | Card detection and FAT directory listing confirmed; writes remain experimental |
| Poweroff and restart | Firmware sequences implemented; physical restart unverified |
| Audio, Bluetooth, FM, camera, lighting, vibration and charging | Hardware research exists; Linux drivers are not implemented |
| Internal flash, cellular and suspend/resume | No Linux support |

See the [hardware guide](hardware.md) for register findings and remaining driver
work. The DSP/modem shutdown protocol is incomplete.

## Run

Install `Nokia6303Linux.jar` and its matching `.jad`, open **Linux**, then
choose **Start Linux**. The package contains the kernel and BusyBox initramfs;
an SD filesystem is optional. The launch screen shows firmware discovery,
loading progress, and boot stages. [Boot stages](boot_validation.md)
map each code to the operation reached.

The launcher's Options menu switches **DMA/PIO** and cycles **24/18/16-bit color**.
The active settings appear on screen and persist between launches. Choosing PIO
and 16-bit color restores the previous display path within the same JAR.

The Linux keypad menu provides:

| Key | Action |
| --- | --- |
| 1 | Uptime and memory |
| 2 | Interrupt counts |
| 3 | Process and memory test |
| 4 | Power and CBUS status |
| 5 | Watchdog status |
| 6 | USB enumeration status |
| 8 | Unmount storage and power off |
| 9 | Unmount storage and restart |
| 0 or centre | Menu |

The separate power button logs an input event. PID 1 feeds the watchdogs and
supervises the shell independently of USB connections.

## USB shell and network

The launcher waits up to 15 seconds for a host to open USB serial before
secure-watchdog, SD and display initialization. It continues immediately when
the host asserts ACM DTR, or after the timeout if no host connects. The kernel
option `rm443_usb.boot_wait_ms` controls this wait (default 0, maximum 30000).

On a Linux host, connect the phone by USB and open the serial device:

```sh
sudo picocom -b 115200 --flow n --imap lfcrlf /dev/ttyACM0
```

Exit picocom with Ctrl-A then Ctrl-X. The input mapping keeps kernel log lines
aligned. At the phone's shell prompt:

```sh
dmesg -n 4
uname -a
free
ps
```

`dmesg -n 4` suppresses routine console messages; `dmesg -n 7` restores them.
The shell provides BusyBox commands, pipes, redirection, and writable RAM files.
Aliases `hw`, `swd` and `usb` read power, secure-watchdog and USB status files.

The phone configures `usb0` as `192.168.7.2/24` and starts Telnet on port 23.
Replace `enu1u3` with the USB network interface name on your host:

```sh
sudo ip address replace 192.168.7.1/24 dev enu1u3
sudo ip link set enu1u3 up
ping -c 3 192.168.7.2
telnet 192.168.7.2
```

This connection has no login, SSH, DHCP, forwarding or default route. Local
MAC addresses are `02:00:00:44:43:02` on the phone and `02:00:00:44:43:01` on
the host.

## Debugging

`rmprobe` opens a reusable `/dev/rm443_probe` session for uploading RAM buffers
and ARM/Thumb routines, calling ROM services, reading memory at an explicit
access width, and retrieving call records. See [probe usage](probe.md) for
the command syntax and execution contract. It runs only when invoked from the
shell; the kernel does not start experiments automatically.

Use `hwinspect` from the phone shell to inspect CPU, memory, driver and card
state. `collect` writes reports and small boot snapshots to a new directory:

```sh
hwinspect summary
hwinspect cpu
hwinspect memory
hwinspect card
hwinspect boot
hwinspect collect /tmp/inspect-1
```

Files under `/tmp` are held in RAM and disappear on reboot. Copy collections
to the host for analysis. Missing kernel interfaces are reported as unavailable,
so the shell script can also be uploaded and run independently of the JAR.

| Interface | Contents |
| --- | --- |
| `/sys/firmware/rm443_inspect/cpu` | CPSR/mode, MIDR, cache type, SCTLR flags, TTBR, DACR, FCSE, context ID and fault registers |
| `/sys/kernel/debug/kernel_page_tables` | Kernel virtual ranges and mapping attributes |
| `/proc/PID/pagemap`, `/proc/PID/smaps` | Process page mappings and memory accounting |
| `/sys/firmware/rm443_inspect/boot_trace` | Loader trace, entry registers and relocation checks |
| `/sys/firmware/rm443_inspect/ramlog` | RAM-console ring |
| `/sys/firmware/rm443_inspect/secure_state` | Secure-watchdog state header |
| `rom_map`, `io_map`, `ffc_map`, `ffe_map` in the same directory | Retained S40 page descriptors |

The snapshot interface requires `CONFIG_RM443_INSPECT` and the boot argument
`rm443.inspect=1`, both enabled by the launcher build configuration. CPU
registers are sampled during the kernel's sysfs read; the output also includes
the calling process's saved CPSR. Page-map files contain descriptors, not the
contents of mapped memory.

`memread ADDRESS LENGTH [OUTPUT]` reads physical memory through read-only
`/dev/mem` mappings. Addresses and lengths accept decimal or `0x` hexadecimal.
Omit `OUTPUT` to stream bytes to standard output, or name a new file:

```sh
memread 0xffff0800 256 | hexdump -C
```

Stream large dumps to the host to avoid filling RAM. On the host, decode a
saved collection with:

```sh
python3 s40/tools/decode_inspection.py COLLECTION_DIRECTORY
```

Periodic PID 1 heartbeat messages are disabled by default. Set the boot argument
`rm443.heartbeat=1` to enable them; watchdog feeding is independent of logging.

## Display

The RAPS LCD driver exposes `/dev/dri/card0` for DRM/KMS and `/dev/fb0` for
framebuffer applications. The standard Linux console uses the full 240×320
screen, with 30×20 characters in the default 8×16 font. Early boot markers
remain visible until DRM takes ownership.

From the USB shell:

```sh
fbtest --info
fbtest
drmtest
lcd status
```

The tests display a pattern briefly and restore the console. Use
`drmtest --benchmark 60` to measure full-screen and partial updates. See
[display](display.md) for formats, transfer statistics and benchmarking.
The USB shell provides [polled and interrupt-driven DMA tests](display.md#dma-tests)
with configurable sizes and an automatic sweep.
Use `lcd pio` / `lcd dma` to change normal rendering, and `lcd rgb565`,
`lcd rgb666` or `lcd rgb888` to compare panel color modes without restarting.

## Storage

The root filesystem is the embedded initramfs. `/tmp` and `/root` are volatile;
files on an SD card persist. Automatic boot logging mounts an existing FAT card
writable for `/Linux/logs/boot-NNNNNN.kmsg`. It never formats the card. The boot
argument `rm443.sdlog=0` disables this startup step and leaves the card unmounted;
the launcher build configuration enables this override. Logging must establish
a durable checkpoint before boot selects DMA; otherwise the display stays on
PIO. `card unmount` stops the logger first.

```sh
card status
card mount
ls /mnt/card
card unmount
```

`card mount` opens FAT read-only. Use `card mount rw` for writes. An ext4 image
inside FAT provides Unix permissions and symbolic links without repartitioning:

```sh
card mount rw
mount -t ext4 -o loop /mnt/card/Linux/linux-data.ext4 /mnt/linux
card unmount
```

`card unmount` unmounts the inner Linux volume first. See
[SD storage](storage.md) for creating an image, partition layouts, and
hardware details. Internal flash has no Linux driver.

## Documentation

| Topic | Guide |
| --- | --- |
| Debug commands, CPU registers and memory dumps | [Debugging](#debugging) |
| Register map, clocks, interrupts and memory | [Hardware](hardware.md) |
| Loader, memory reservations and stage codes | [Boot sequence](boot_validation.md) |
| DRM/KMS, framebuffer and panel transport | [Display](display.md) |
| SD cards, FAT and ext4 images | [Storage](storage.md) |
| CBUS, watchdogs and shutdown | [Power](power.md) |
| RTC, calendar origin, alarms and proposed Linux RTC support | [RTC and alarms](rtc.md) |
| ADC calibration, battery monitoring and charging | [Battery and charging](battery_charging.md) |
| LCD/keypad lighting and panel cold initialization | [Lighting](lighting.md) |
| Ambient-light sensor, calibration and brightness policy | [Ambient light](ambient_light.md) |
| Vibration motor control and proposed force-feedback support | [Vibration](vibration.md) |
| BCM2048, H4+ transport and proposed Linux integration | [Bluetooth](bluetooth.md) |
| BCM2048 FM tuning, RDS and audio routing | [FM radio](fm_radio.md) |
| Speakers, headphones, music, tones and proposed ALSA integration | [Audio](audio.md) |
| Camera, Julie processor, capture and flash | [Camera and flash](camera_flash.md) |
| Toolchain and incremental build | [Kernel build](../kernel/README.md) |
| Component checks | [Tests](../tests/README.md) |
