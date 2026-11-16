# Boot sequence and validation

The launcher discovers S40 services, validates the ARM926 CPU state and MMU
policy, translates the embedded Image, and allocates transition storage.
It then switches to private translation, stops known device writers, relocates
Image, and enters Linux. Reset is required to return to S40.

## Memory layout

Linux uses 30 MiB at PA `0x10200000..0x11ffffff`. Image starts at `0x10208000`;
the first `0x8000` bytes include early Linux page tables. The low 2 MiB is
excluded because it contains DSP shared memory and was the location of observed
kernel corruption.

The launcher requests 80 KiB and aligns a 64 KiB transition block to 16 KiB.
Every page of the allocation and Image is translated and checked for physical
contiguity. The transition block must avoid the Image source, destination and
BSS. Its layout is:

| Block offset | Purpose |
| --- | --- |
| `0x0000..0x1000` | Device tree |
| `0x8000..0xcc00` | L1 and three coarse translation tables |
| `0xd000..0xe000` | ARM926 transition code |
| `0xe000` | Versioned boot trace |
| `0xe100` | Relocation plan |
| `0xf000..0x10000` | RAM log |

The device tree also reserves the firmware warm-reset policy page when the
loader discovers and validates it. That page is checked against Linux's early
page tables, kernel footprint, Image source, and transition block. Discovery
uses code relationships and live page tables on both 08.90 and 10.12.

`kernel/build/image-layout.json` records the matching Image file size and
`_end - _text` footprint. FNV-1a checks compare the embedded Image with its build
value before takeover, again through physical addressing, and after relocation.
Trace format 3 records the results and completed shutdown operations.

## CPU transition

The private tables map the transition code at its firmware virtual address and
physical address, plus LCD/GPIO pages for stage output. The code cleans caches,
installs the private tables, branches to its physical alias, then disables the
MMU. It does not use the old firmware stack after this switch.

Before copying Image, the transition masks IRQ/FIQ, clears the DSP release bit,
stops six linear and four circular DMA channels, and disables USB requests and
interrupts. Polling is bounded. A failure records its stage and stops before
relocation. DSP control readback confirms the write was accepted; the complete
runtime DSP/modem shutdown protocol remains unresolved.

The overlap-safe copy clears BSS and enters Linux in ARM/SVC state with IRQ/FIQ
masked, MMU and data cache off, `r0=0`, `r1=~0`, and `r2=DTB PA`, following the
[ARM boot protocol](https://docs.kernel.org/arch/arm/booting.html).

## Visible stages

Stages identify the operation reached. Record the screen from opening the app
when investigating a failed launch; short stages may fall between video frames.

| Code | Operation |
| --- | --- |
| `10` | Firmware discovery and native bootstrap |
| `20` | Native loader installed; Java heap restored |
| `21` | Native payload allocation |
| `22` | Payload copy |
| `23` | Native application entry |
| `30` | CPU/MMU validation |
| `31` | Payload layout validation |
| `32` | Transition allocation |
| `33` | Mappings and device tree prepared |
| `34` | Transition code synchronized; LCD preflight next |
| `35` | Copied ARM code draws directly through LCD PIO |
| `36` | LCD preflight returned; takeover next |
| `37` | IRQ/FIQ masked |
| `38` | Data cache cleaned; caches disabled |
| `39` | Private page tables active |
| `3C` | Embedded Image fingerprint mismatch |
| `3D` | Uncached LCD/GPIO aliases could not be verified |
| `3E` | LCD preflight or display restoration timed out |
| `40` | Physical transition, MMU/caches off |
| `41` | Device shutdown and source Image check completed |
| `42` | Image relocated, BSS cleared, destination check completed |
| `43` | Kernel entry |
| `4C` | Physical source Image fingerprint mismatch |
| `4D` | Relocated Image fingerprint mismatch |
| `4F` | Device shutdown failed |
| `50` | Kernel early board initialization |
| `51` | Watchdog/power initialization |
| `52` | Watchdog/power initialization completed |
| `53` | DMA stop and IRQ initialization completed |
| `54` | Timer initialization completed |
| `55` | DMA driver probe entered |
| `56` | DMA driver registered; idle channel interrupts masked |
| `57` | DRM display probe entered |
| `58` | LCD transport initialized |
| `59` | DRM mode setup completed; panel takeover next |
| `5F` | Early power or DMA setup failed |
| `80` / `90` | Poweroff / restart entered |
| `81` / `91` | Device shutdown completed |
| `82` / `92` | Final poweroff / reset callback |
| `8F` / `9F` | CPU continued after the final hardware command |

Java/native stages briefly pause while S40 still services the hardware. No
pauses are introduced after takeover. LCD transfers have bounded waits, so a
stalled display need not stop boot. Stage `10` covers the forged-array interval
without UI calls, which would risk corrupting Java state. DRM takes ownership
of the whole screen and disables further direct marker writes. Later
poweroff/restart stages still appear in the kernel log.


The [component tests](../tests/README.md) exercise the DT writer, relocation,
shutdown and marker code with synthetic inputs. They do not establish device
stability; current hardware limitations are in the [usage guide](usage.md).
