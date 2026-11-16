# Release history

Current installation instructions and hardware limitations are in
[README.md](README.md) and the [usage guide](docs/usage.md).

## 0.8.17

Add paged RAM objects, flash-backed views, temporary virtual mappings and memory
budgets to `rmprobe`. Stream buffer transfers without a second full image in
phone RAM. ROM calls still require physical argument addresses.

## 0.8.16

Preserve the fourth callback argument across the Thumb-to-ARM trampoline.

## 0.8.15

Add `/dev/rm443_probe` and `rmprobe` for RAM buffers, ROM calls, uploaded
ARM/Thumb routines, explicit-width memory reads and execution records.

## 0.8.14

Disable CPU idle sleep with `nohlt` while the RAPS wakeup path is unresolved.
Idle power consumption increases; sustained handset stability remains unconfirmed.

## 0.8.13

Fix a USB console request lifetime race during reset or disconnect.

## 0.8.12

Complete the first secure-watchdog refresh before MMC/DRM initialization.
Disable cursor blinking and SD request tracing in the launcher configuration.

## 0.8.11

Disable and report peripheral interrupt sources that reassert 4096 times in a
single dispatch, allowing other work and watchdog servicing to resume.

## 0.8.10

Add bounded SD polling and optional request tracing. Select polling in the
launcher to avoid relying on SD interrupt delivery.

## 0.8.9

Start USB before secure-watchdog, MMC and display initialization. Wait up to
15 seconds for the host to open serial; `rm443_usb.boot_wait_ms` controls the wait.

## 0.8.8

Add `rm443.sdlog=0` and enable it in the launcher. Automatic card mounting and
logging are disabled; startup display transport remains PIO.

## 0.8.7

Add optional persistent kernel logging on FAT, `hwinspect` collections and
read-only `memread`. Fix MMC FIFO servicing and write deadlines; select a
1-bit/400 kHz SD profile. Disable periodic heartbeat messages by default.

## 0.8.6

Add persistent DMA/PIO and 24/18/16-bit display choices, adjustable with `lcd`.
The 24-bit output path corrects the green tint seen on the tested handset.

## 0.8.5

Use noncached GEM DMA framebuffer mappings to fix pixel streaks caused by cached
aliases. Extend `dmatest` with configurable rectangles and a size sweep.

## 0.8.4

Add explicit polled and interrupt-driven LCD DMA tests while normal rendering
uses PIO.

## 0.8.3

Use PIO rendering while investigating the DMA startup failure on hardware.

## 0.8.2

Mask DMA interrupts while channels are idle and restore global DMA setup.
Add boot stages 55–59 for DMA and display initialization.

## 0.8.1

Add DMAengine LCD transfers, damage-update timing and `drmtest --benchmark`.
Reduce fbdev update batching from 50 ms to 10 ms.

## 0.8.0

Replace the custom text console with DRM/KMS, `/dev/fb0` and full-screen fbcon.
Support RGB565 and XRGB8888 buffers, damage updates, `fbtest` and `drmtest`.

## 0.7.9

Continue the inherited secure watchdog through the ROM service and expose its
status with `swd`. Add an SD reset fallback clock; card detection and FAT directory
listing then worked on the tested handset.

## 0.7.8

Enable the SD controller clock before waiting for reset completion.

## 0.7.7

Rename the application and package to Linux / `Nokia6303Linux`.

## 0.7.6

Restore USB endpoint interrupts after reset without discarding pending SETUP
requests. This enables the USB serial shell on the tested handset.

## 0.7.5

Correct USB register access widths and EP0 FIFO handling. Add the key-6
USB enumeration report.

## 0.7.4

Preserve the firmware warm-reset policy page, add ordered storage shutdown,
and correct the USB transceiver/pull-up sequence.

## 0.7.3

Move Linux to the 30 MiB bank at `0x10200000`, excluding DSP shared memory after
kernel corruption was observed in the low 2 MiB. Verify Image integrity before
and after relocation, and finish stopping linear DMA channels.

## 0.7.2

Restore LCD GPIO configuration for direct boot markers and add display preflight.

## 0.7.1

Add visible boot stages and correct direct LCD transfer handling.

## Earlier releases

| Version | Changes |
| --- | --- |
| 0.7.0 | SD/MMC, FAT/ext4, card detection, explicit read-only/read-write mounts and ordered unmounts |
| 0.6.0 | Replace the small allocated arena with DDR takeover and overlap-safe kernel relocation; add tmpfs, loop devices and `memcheck` |
| 0.5.0 | Full-speed USB ACM/ECM, BusyBox/musl, supervised shells and chunked launcher resources |
| 0.4.0 | RAPS/AVilma watchdogs, bounded CBUS access, power key and shutdown callbacks |
| 0.3.0 | IRQs, timers, RAM/LCD consoles, keypad and PID 1 |
| 0.2.0 | ARM926 transition and initial Linux entry |
