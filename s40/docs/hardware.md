# Nokia 6303 classic hardware

The RM-443 uses a RAPS application processor with AVilma and Betty support
chips. This reference combines the service schematic and recovered 08.90/10.12
firmware contracts. Current supported features are listed in the
[project README](../README.md); an identified peripheral is not necessarily
implemented by this Linux port.

## Processor and memory

The [RM-443 service schematic, version 1.0](https://manuals.plus/m/e24d719873f72f79366308698e1927f2a3d2ca5373e43706ace052d4acdbb051.pdf)
identifies RAPS_V3.03-PA and J58F0046L0Y1GE stacked memory: 64 MiB NOR and
32 MiB DDR. The handset MIDR is `0x41069265`, ARM926EJ-S revision 5 with
VIVT instruction/data caches. The port targets little-endian ARMv5TE without VFP.

The MCU runs at 224 MHz in the measured awake, USB-connected S40 state. The
recovered PLL setting uses a 19.2 MHz reference, multiplier 35 and divisor 3.
NOLO reports DSP 235.2 MHz and DRAM 112 MHz; those are bootloader values rather
than measurements of Linux clock changes. Suspend and dynamic clock scaling
are not implemented.

| Region | Meaning |
| --- | --- |
| PA `0x10000000..0x11ffffff` | 32 MiB DDR |
| PA `0x10200000..0x11ffffff` | Current 30 MiB Linux bank |
| VA `0xd8000000..0xda000000` | S40 resolver's RAM envelope; pages need translation |
| VA `0xd0000000..0xd0003fff` | S40 L1 page-table alias, protected by domain 0 |
| VA `0xf0100400` onward | S40 MCUSW code mapping |
| PA `0x0800xxxx` | Internal scratch/stack used by NOLO |

S40's RAM allocation is mapped at `0xd8000000`, but its physical start is chosen
by the boot allocator. Subtracting `0xc8000000` from arbitrary S40 pointers is
not a translation method. Native-heap free space also does not describe all
physical RAM: firmware data, stacks, Java, DSP and device buffers occupy other
regions.

Linux leaves the low 2 MiB reserved because it contains DSP shared storage and
has shown writes into memory reclaimed by an earlier full-DDR configuration.
The Image is relocated to `0x10208000`; transition memory and the discovered
firmware reset-policy page are also reserved. See
[boot validation](boot_validation.md) and [handoff](power.md#physical-handoff-before-full-ddr-reuse).

## MMU translation

The loader discovers the table walker and validates its instruction/helper
relationships. It does not select function addresses by firmware version.
The recovered table policy is:

```text
L1 descriptor VA = 0xd0000000 + 4 * (target_va >> 20)
coarse L2 VA     = (l1 & 0xfffffc00) + (0xd0000000 - TTBR)
fine L2 VA       = (l1 & 0xfffff000) + (0xd0000000 - TTBR)
```

L1 is 16 KiB aligned. Reading these aliases requires temporarily setting domain
0 to client: `(DACR & ~3) | 1`. The MMU probe validates TTBR and the helper
relationships, masks IRQ/FIQ, reads the tables, and restores DACR and interrupt
state. It makes no firmware calls while the domain setting is changed. Hardware
abort recovery is not implemented.

## Peripheral mappings

The fixed mapping table is shared by the inspected firmware builds. Register
addresses in the device guides are physical unless otherwise stated.

| Physical base | S40 virtual base | Bytes | Known use |
| --- | --- | ---: | --- |
| `0x20100000` | `0xffb00000` | `0x100000` | Firmware trace; debug UART unresolved |
| `0x40000000` | `0xffc00000` | `0x1000` | Interrupt controller |
| `0x20010000` | `0xffd10000` | `0x1000` | Unidentified |
| `0x20020000` | `0xffd20000` | `0x3000` | Serial, SPI, CCP, LCD and related blocks |
| `0x20030000` | `0xffd30000` | `0x7000` | CBUS, GPIO, clocks, SD and USB |
| `0x20040000` | `0xffd40000` | `0x4000` | Timers, watchdog, IPC and DMA |
| `0x20050000` | `0xffd50000` | `0x3000` | Unidentified |
| `0x14000000` | `0xffe00000` | `0x1000` | DRAM setup window |

Some reads have side effects, notably hardware semaphore acquisition. These
mappings are not a list of registers suitable for indiscriminate dumping.

## Interrupts and timekeeping

The RAPS interrupt controller has 226 sources. Linux creates a linear IRQ domain,
disables inherited routes, selects default configuration 2, clears inherited
controller lock counts and enables IRQ delivery. FIQ is unused.

| INTC offset from `0x40000000` | Operation |
| --- | --- |
| `+0x08` | Read pending source; `0xffffffff` means none |
| `+0x0c` | Write source index to enable |
| `+0x10` | Write source index to disable; `0xffffffff` disables all |
| `+0x20`, `+0x24` | Select source and write configuration |
| `+0x88/+0x8c`, `+0x94/+0x98` | Critical-section entry/exit controls |
| `+0x90` | IRQ-entry state; cleared on exit |

Reading the vector does not acknowledge a peripheral's level condition. The
Linux level handler masks the source while the device handles and clears it.

`timer.c` implements the 32-bit 32768 Hz clocksource by writing zero to
`0x20040494`, then reading `0x20040490`. It cancels ten inherited countdown
channels and owns high-speed channel 2 / IRQ36 as a 9.6 MHz clockevent.
One-shot programming writes `0x200401a8`; zero cancels its level. Periodic
rearming adds the interval through `0x200401c8`, preserving the previous expiry
phase. DT supplies the event frequency. Timer interrupts and advancing uptime
have been observed on the handset.

## Implemented device paths

- **Power:** RAPS and AVilma watchdogs, CBUS arbitration, power key, reset and
  poweroff are in `kernel/overlay/arch/arm/mach-nokia-raps/`. The separate secure
  watchdog retains a ROM call bridge. See [power](power.md).
- **Display:** the 240×320 DRM/KMS driver uses PIO commands and optional linear
  DMA pixel transfers at `0x20022200`. It inherits panel power and timing from
  S40. See [display](display.md) and [lighting](lighting.md).
- **Keypad:** GPIO39–42 drive columns; GPIO32–37 sample active-low rows.
  `rm443_keys.c` uses set/clear aliases, 20 ms polling and two equal samples for
  debounce. Numeric/navigation/soft/volume/call keys have Linux input codes.
  Ghost-key handling and wake from suspend are unimplemented.
- **USB:** the full-speed PIO controller and Betty transceiver expose ACM and
  ECM gadget functions. Physical enumeration and bidirectional serial traffic
  are confirmed; physical Ethernet traffic remains unverified.
- **SD:** the RAPS MMC driver uses PIO with inherited pinmux. Physical card
  enumeration, FAT mounting and directory reads are confirmed; writes and ext4
  operation remain unverified. See [storage](storage.md).

The six linear DMA channels use `0x20040c08 + channel * 0x400` command
registers; four circular channels use `0x20042440 + channel * 4`. The handoff
stops both groups before relocating Linux and clears linear ENABLE after stop.
This is not proof that every DSP/modem or peripheral-local bus master is idle.

## Unimplemented peripherals

| Area | Recovered interface / remaining dependency |
| --- | --- |
| Backlight and keypad LEDs | Betty PWM/shared boost, LP5521 channels; [lighting](lighting.md) |
| Ambient light | TSL2563; needs RAPS I²C and supply ownership; [sensor](ambient_light.md) |
| Battery and charging | Shared PMIC ADC/calibration and Betty charge controls; [reference](battery_charging.md) |
| RTC | Calendar, retention and alarm registers; [reference](rtc.md) |
| Bluetooth | BCM2048_B0, Nokia H4+, serial and circular RX DMA; [reference](bluetooth.md) |
| Audio | DSP PCM/tone service, AVilma routes and TPA6130A2 headphones; [reference](audio.md) |
| Vibration | DSP F3 motor commands; [reference](vibration.md) |
| FM radio | BCM2048 I²C tuner, shared BT/FM power and audio route; [reference](fm_radio.md) |
| Camera and flash | Julie processor over SPI/CCP; [reference](camera_flash.md) |
| Internal flash, SIM and cellular | No Linux drivers or modem service |
| Suspend/resume | Clock/rail retention, wake sources and peripheral restore unresolved |

Additional drivers must share the existing CBUS, pin and clock owners rather
than independently reprogramming overlapping resources. See
[shared services](power.md#shared-services-needed-by-additional-devices).
