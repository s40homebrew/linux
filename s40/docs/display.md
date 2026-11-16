# RAPS LCD

The display uses DRM/KMS with a fixed 240×320 mode, one primary plane and
RGB565/XRGB8888 buffers. `/dev/dri/card0` accepts dumb-buffer, modeset and atomic
requests; `/dev/fb0` provides fbdev access. Rendering and conversion run on the
CPU. There is no GPU or render node.

The current build disables automatic SD logging and starts on **PIO**. A build
with logging enabled applies the launcher's selected DMA transport only after
the logger's durable checkpoint succeeds. Manual `lcd dma` remains available.
The default panel color selection is 24-bit. Physical tests have confirmed PIO
output and full-frame DMA, with neutral whites using 24-bit panel output.

fbcon provides a 30×20 text console with an 8×16 font. Its default RGB565 buffer
uses 150 KiB before alignment/overhead. The early boot marker releases the screen
when DRM probes; later checkpoints use the kernel log.

## Display settings

The launcher's Options menu stores DMA/PIO and 24/18/16-bit color choices.
From the USB shell:

```sh
lcd status
lcd pio
lcd dma
lcd rgb565
lcd rgb666
lcd rgb888
```

Color selection changes the panel wire format, not the DRM/fbdev buffer format.
RGB666 and RGB888 send R,G,B bytes using the stock raw COLMOD values `06` and
`07`. RGB565 sends two bytes with COLMOD `55`. Gamma and orientation remain as
S40 initialized them. Repaint with `fbtest` after changing formats.

Transport changes serialize with updates. A recoverable DMA error disables DMA
and replays the rectangle through PIO. If the controller cannot stop, the driver
keeps its buffer mapped and blocks further LCD access. `lcd status` reports the
effective transport and errors. Sysfs controls are under
`/sys/class/drm/card0/device/`: `transport` and `pixel_format`.

## Drawing and diagnostics

```sh
ls -l /dev/dri /dev/fb0
fbtest --info
fbtest --seconds 10
drmtest --seconds 10
drmtest --rgb565 --seconds 10
```

`fbtest` queries geometry and draws through fbdev. `drmtest` uses KMS and tests
a partial rectangle as well as full-screen content. Both restore the console
on exit and run independently of PID 1's watchdog feeding.

Framebuffer applications must query `FBIOGET_VSCREENINFO` and
`FBIOGET_FSCREENINFO`, honor pitch/channel layout and mmap the buffer. Applications
owning a VT should switch to `KD_GRAPHICS` and restore its previous mode. KMS
applications should use advertised formats and damage clips. The panel retains
pixels; there is no periodic full-screen refresh.

For an isolated DMA diagnostic, leave normal updates on PIO:

```sh
lcd pio
lcd rgb565
dmatest poll
dmatest irq
dmatest status
```

The default test draws a 16×16 square at the bottom right: green for polling,
magenta for IRQ completion. Other sizes use a pattern generated directly in the
kernel transfer buffer, bypassing framebuffer conversion:

```sh
dmatest poll 64 64
dmatest irq 240 320
dmatest sweep poll
dmatest sweep irq
```

Widths are 1–240, heights 1–320, with at least two pixels. Sweeps stop on the
first returned error. Status includes the result and register snapshots; it is
also available through `/sys/class/drm/card0/device/dma_test`. Polled completion
waits for DMA source/count/idle state before LCD completion, rather than trusting
an already-set LCD status bit.

## Update path

`raps_drm.c` converts damaged pixels into a packed buffer: 153,600 bytes for a
full RGB565 frame or 230,400 bytes for RGB666/RGB888. Selected DMA handles
payloads of at least 64 bytes. Commands, small updates and unaligned tails use
PIO transactions of up to four bytes. The DMA provider owns six linear channels;
LCD hardware request is **9**, distinct from firmware logical endpoint 18.
Idle interrupts remain masked; active IRQ transfers unmask after programming
and mask before stop.

Framebuffer storage uses GEM DMA allocation and matching write-combined user and
kernel mappings. This avoids conflicting cached aliases on ARM926; allocating
this storage does not itself select DMA transport. fbdev mmap updates are
batched for 10 ms, while direct KMS updates have no batching delay.

The interface is at physical `0x20022200`, with command/count at `+4`, data at
`+16` and transfer-complete bit 3 at `+8`. LCD pin activation uses GPIO set/clear
aliases at `0x20031c00`: `+0x28=0xffff`, `+0x24=0x5555`, `+0xe8=0xff0000`.
These select the LCD mux fields and clear direction bits for pins 16–23.

Optional PIO data pacing is available for transport diagnosis:

```sh
lcd pio
echo 1 > /sys/class/drm/card0/device/pio_delay_us
fbtest --seconds 10
```

The value is 0–10 microseconds per FIFO word; zero restores the default.
Commands and DMA data are unaffected. One microsecond adds at least 38.4 ms to
a full RGB565 frame, or 57.6 ms to a three-byte-format frame.

## Benchmarking

```sh
drmtest --benchmark 60
drmtest --rgb565 --benchmark 60
cat /sys/class/drm/card0/device/transfer_stats
```

Each run updates a full frame and a 31×19 rectangle. `end-to-end` includes filling
userspace pixels; `submit+transfer` includes kernel conversion and completed
transfer. The accepted count is 1–300. These measure update cost, not panel
refresh rate.

Measured XRGB8888-to-RGB888 DMA runs took about 83 ms end-to-end and 38 ms
submit/transfer per full frame; small updates took about 1.1 ms and 0.74 ms.
These are handset samples, not throughput guarantees. `transfer_stats` reports
transport, format, update/error counts, bytes and driver update times, including
console drawing.

## Source and limitations

The driver is in `kernel/overlay/drivers/gpu/drm/raps/`: `raps_drm.c` owns KMS
and conversion; `raps_lcd.c` owns panel commands/transfers. The DMA provider is
`kernel/overlay/drivers/dma/raps.c`; utilities are in `kernel/userspace/display/`.

S40 supplies panel power, orientation and bus timing. Firmware supports Sharp
and Seiko variants, but the exact fitted controller is not identified. Cold
initialization, backlight control, tearing synchronization and suspend/resume
are unimplemented. KMS blanking does not switch panel rails or the backlight.
See [lighting](lighting.md) for initialization/backlight contracts and
[ambient light](ambient_light.md) for the separate sensor.

DRM updates require a working scheduler; a panic can prevent the final screen
update. Use USB/kernel logs as well as the display when diagnosing failures.
