# RAPS and AVilma power management

The board driver owns the RAPS and AVilma watchdogs, CBUS transactions, the
AVilma power key, poweroff and reset. A separate secure watchdog continues an
inherited ROM service through a retained call bridge. Cold power-on, charging,
PMIC interrupt dispatch and suspend/resume are not implemented.

## Secure watchdog

PA_SWD is independent of the two ordinary watchdogs. Command `0x1f` uses a
16-bit renewal interval at request offset 4 and returns
`{status:u32, active_interval:u16, maximum_interval:u16}`. Zero keeps the current
interval; a nonzero request is capped at the maximum. Existing code calls this
field `token`/`next_token`, but it is an interval rather than an authentication
token.

Linux makes its first request one second after initialization. Successful
renewals are scheduled thirty seconds before the returned deadline, or halfway
through intervals at most thirty seconds. Transport and PA status must both be
one. Failed replies stop renewal and remain visible in diagnostics.

The launcher discovers the wrapper, interval, PA table, callback and allocator
layouts by validated instruction relationships. It reserves the original 8 KiB
work buffer and live callback allocations at their physical addresses. Final
state is captured with IRQ/FIQ masked after the last S40 checkpoint.
A separate 128 KiB reservation holds private page tables, call code, stack,
messages and callback allocation space.

The ARM926 bridge enters ROM with physical arguments and private translations.
Its callback preserves existing allocations and serves new requests from four
16 KiB slots without calling the stopped S40 heap. Persistence requests are
counted, but no PMM flash writer is implemented. The bridge restores Linux's
translation, stack and interrupt state; ordinary watchdogs are refreshed before
entry. Full physical renewal reliability remains unverified.

Use `swd`, key **5**, or `/sys/kernel/rm443_secure_watchdog/status` for results.
`hw` includes both ordinary and secure watchdog status. Successful service counts
should advance over returned deadlines; initialization/results also appear in
`dmesg`.

## Recovered contract

These are physical addresses. The inspected S40 builds use corresponding
`0xffd...` peripheral aliases.

| Mechanism | Operation |
| --- | --- |
| CBUS, `0x20030700` | Enable `+0x04`, busy `+0x08`, read command `+0x18`, write command `+0x1c`, data `+0x20`, bit count `+0x2c`, divider `+0x30` |
| CBUS semaphore, `0x20040204` | A positive signed read acquires a token; write `0xffffffff` to release |
| RAPS watchdog | Write zero to `0x20040504` to reload; `327680` at `0x20040508` selects ten seconds |
| AVilma watchdog | CBUS chip 1, register `0x17`, low six bits; firmware reload value 32 |
| Poweroff | Write zero to the AVilma watchdog field |
| Ordinary reset | Write 2 to `0x20032d14` |
| Reset status | Read `0x20032c18`; reset-cause bit meanings remain unresolved |
| Power key | AVilma register `0x16`, bit 5 active low |

Reading hardware semaphore registers acquires ownership; broad MMIO dumps are
not observational. There is no verified RAPS disable operation, and zero at
`0x20040508` must not be treated as one. The driver exposes no countdown read
through `WDIOC_GETTIMELEFT`.

## Linux implementation

`kernel/overlay/arch/arm/mach-nokia-raps/rm443_power.c` refreshes the inherited
RAPS deadline at the start of board IRQ setup. It checks PMIC family IDs,
reloads AVilma to 32 seconds, establishes the ten-second RAPS period and masks
AVilma IRQ delivery without acknowledging pending latches. Failure halts with
interrupts masked.

Each CBUS transaction holds a raw spinlock and the hardware semaphore.
Acquisition and busy waits have 100,000-read budgets; errors release acquired
ownership. Masked writes preserve unrelated fields. The inherited divider is
left unchanged.

`/dev/watchdog0` represents both deadlines. A ping reloads AVilma before RAPS,
so a failed CBUS transfer cannot report that both devices were refreshed.
The watchdog core feeds hardware during the transition to PID 1, which then
supplies keepalives. The logical timeout defaults to 32 seconds, with a ten-second
maximum hardware heartbeat and `nowayout`. The feed path validates **1..63 before
MMIO**: truncating an invalid value of 64 to six bits would request poweroff.

The DT node `power@20030700`, compatible `nokia,rm443-power`, supplies CBUS,
semaphore, watchdog, reset-status and reset-control resources in that order.
`/sys/devices/platform/20030700.power/status` reports PMIC IDs, boot/status
registers, watchdog readback, timeout, transaction/error counts, inherited IRQ
mask, divider and raw RTC registers 3/4. It does not interpret or set a date.
`rm443_power_key.c` polls every 100 ms with two-sample debounce and emits
`KEY_POWER`; failed reads do not manufacture a release event.

## Shared services needed by additional devices

RTC, battery, charging and lighting share PMIC state with watchdogs and USB.
`rm443_cbus_read()` and `rm443_cbus_update()` serialize individual transfers.
They are not an ADC conversion service or PMIC interrupt controller:

- ADC selection, conversion and result collection need one operation owner.
  A bus lock per transfer cannot prevent another client changing the channel.
- Command and write-one-to-clear registers need their actual write semantics;
  ordinary read-modify-write can acknowledge unrelated pending events.
- Conversion delays and brightness ramps must not hold the raw spinlock with
  interrupts disabled.
- GPIO, pinmux and clocks already have overlapping consumers. Keypad reserves
  `0x20031cc0..0x20031da3`, USB owns a pinmux word at `0x20031c68` and a clock
  slice at `0x20032c80` of size `0x104`. New providers must migrate or share
  those resources.

Both DT producers, `kernel/make_dts.py` and `boot_probe/native/boot_fdt.c`, must
agree on new resources; the launcher FDT buffer is 4096 bytes. New subsystem
sources also need explicit integration in `kernel/prepare.py` and
`kernel/rm443.config`.

Vibration additionally depends on the DSP F3 service. Its AVilma wiring does
not establish a direct CBUS motor command. See [vibration](vibration.md),
[audio](audio.md), [RTC](rtc.md), [battery](battery_charging.md) and
[lighting](lighting.md) for the recovered device contracts.

## Physical handoff before full-DDR reuse

`handoff/rm443_quiesce.inc` runs from private identity code with MMU/caches off
and IRQ/FIQ masked, without the old stack or firmware calls. It clears the DSP
release bit at `0x20032d18`, stops six linear and four circular DMA channels,
and disables USB interrupt/DMA requests and pullup/connect. It preserves DDR,
shared clocks and PMIC rails. RAPS is refreshed before and after; AVilma keeps
running until early kernel handover.

Each poll is bounded to 100,000 reads. On failure the caller records the result
and parks before Image relocation. Return codes `0x101..0x106` identify linear
channels, `0x201..0x204` circular channels, `0x301` DSP control write acceptance,
and `0x401` USB disable. Completed-stage bits are DSP=1, linear DMA=2, circular
DMA=4 and USB=8.

DSP control readback is not an idle acknowledgement. The recovered shared-copy
window covers offsets `0x140000..0x1c0000` from physical `0x10000000`, and the
writer of previously observed corruption was not identified. Linux therefore
keeps the low 2 MiB reserved. Complete DSP/RF shutdown remains unresolved.

Linear DMA stop waits for COMMAND to become zero, then clears ENABLE. The
firmware's circular stop can drain a pending element through a consumer
callback; that cannot be replayed with an invented firmware pointer. The port
has no general proof that every peripheral-local bus master is quiescent.

## Reset and shutdown

The launcher discovers and reserves the firmware's 4 KiB warm-reset policy page
through validated code/data relationships and live translation. The restart
callback writes ordinary reset reason 3, clears the adjacent reset-loop counter,
refreshes watchdogs, masks interrupts, synchronizes caches/writes, then sends
reset command 2. Missing discovery is reported; no guessed RAM address is used.
Poweroff refreshes RAPS before writing the AVilma watchdog-zero command.

PID 1 requires filesystem synchronization/unmount to succeed before reset or
poweroff, and feeds watchdogs while waiting. Its storage helper uses `/dev/null`
and `/dev/kmsg`, so shutdown does not depend on an attached USB console.

| Restart | Poweroff | Checkpoint |
| --- | --- | --- |
| `90` | `80` | Kernel shutdown entered |
| `91` | `81` | Device shutdown completed |
| `92` | `82` | Final hardware callback entered |
| `9F` | `8F` | CPU still running one second after command attempt |

After DRM takes ownership, checkpoints go to the log rather than direct LCD
writes. Physical S40 startup after reset, nearly expired watchdogs at handoff
and suspend/resume still need hardware validation.
