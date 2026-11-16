# RTC, calendar retention and alarms

AVILMA provides a CBUS RTC with time of day and an eight-bit elapsed-day
counter. S40 adds a calendar origin from persistent records. The Linux port
has no RTC driver or alarm device; its power diagnostics read registers 3 and
4 separately and do not provide a coherent clock.

The following contract was recovered from MCUSW 10.12 and 08.90. Physical
counter rollover, clock-setting edges, retention and alarm wake remain
unverified.

## Register contract

All registers are on **AVILMA, CBUS chip 1**.

| Register | Fields or behavior |
| --- | --- |
| `01` | Pending events: alarm/day/minute/second in bits 5/4/3/2; write-one-to-clear |
| `02` | Interrupt masks; one masks an event |
| `03` | Elapsed days in bits 15:8, seconds in bits 5:0; day `FF` is invalid |
| `04` | Live hours in bits 12:8, minutes in bits 5:0; read-only in firmware metadata |
| `05` | Hour/minute window: bit 15 selects clock setting (1) or alarm access (0) |
| `05` | Minutes in bits 5:0, hours in bits 12:8 |
| `05` | Initialization sets bit 6; bits 14:13 accept 0..3; physical meanings unresolved |
| `06` | Calibration word; read helper returns its low byte |
| `0D` | RTC initialization clears bit 0; wider meaning unresolved |

Pending-event acknowledgement writes only the intended bits. Using an
ordinary read-modify-write on register 1 can clear unrelated pending events.
The port's current `rm443_cbus_update()` is therefore insufficient for this
operation. See [power](power.md) for shared CBUS ownership.

### Coherent time reads

S40 reads registers 3/4 twice and accepts matching pairs. A changed pair
becomes the next candidate; three mismatches fail. The accepted fields form:

```text
elapsed_seconds = days * 86400 + hours * 3600 + minutes * 60 + seconds
```

A Linux reader must validate the final accepted fields, including day 255,
and propagate transfer errors. There is no recovered hardware latch command,
and the physical day-counter wrap/saturation rule is unknown.

### Clock setting

The setter selects register 5 bit 15, then writes minutes and hours. It does
not explicitly write seconds or days. S40 maintains additional software state
to support seconds-accurate setting. The physical commit/reset behavior needs
measurement before implementing `set_time`.

Clock setting and alarm access share the register-5 window and must be
serialized as complete operations.

## Calendar origin and retention

S40's calendar library uses **1980-01-01 00:00:00** as its epoch and accepts
years 1980..2079. That is a software convention, not a hardware date register.
Its PMM records are:

| Record ID | Size | Role |
| --- | --- | --- |
| `0x80001` | 16 bytes | Calendar copy 1 |
| `0x80002` | 16 bytes | Calendar copy 2 |
| `0x80008` | 8 bytes | Saved alarm and calibration fingerprint |

Calendar records contain a seconds value at offset 0, date-origin seconds at
4 and validity at 12. Startup compares those fields in both copies and also
checks alarm/calibration fingerprints and retained boot state. The stored
calendar's timezone convention is unresolved.

Day maintenance adds elapsed whole days to the software origin, invokes the
save path, then clears register 3 mask `FF00`. Clearing that field without
advancing a trusted origin loses days. The recovered ordering does not
establish that the asynchronous flash save completes before the clear.

Linux cannot reconstruct an absolute date from registers 3/4 alone after a
cold start. A usable RTC needs a validated copied origin at handoff or
Linux-owned durable origin storage. Internal PMM access is not implemented.
Changing alarm or calibration also changes S40's retention fingerprint and
may cause S40 to reject its saved date on a later boot.

## Alarm behavior

Alarm access selects register 5 bit 15 = 0. A minute value of **61** disables
the alarm; S40 represents this as the software sentinel **108000**. The
hardware path exposes hours and minutes only, with no date or seconds field.

| RTC event | PMIC event number | Pending/mask bit |
| --- | --- | --- |
| Alarm | 10 | `0020` |
| Day | 11 | `0010` |
| Minute | 12 | `0008` |
| Second | 13 | `0004` |

The bulk mask field is `003C`: after shifting, its low bit means second,
followed by minute/day/alarm. This is the reverse of S40's local RTC selector
numbering.

AVILMA's shared interrupt uses **GPIO74 / RAPS IRQ216**, GPIO bank physical
base **`0x20031dc0`**, bit **`0x0400`**. PMIC event acknowledgement and GPIO
latch acknowledgement are separate. The current Linux port masks AVILMA
interrupt delivery and lacks the shared PMIC/GPIO IRQ infrastructure.

A future RTC alarm should expose minute precision and an explicit date
horizon. Suspend wake and power-on alarms require separate hardware support;
neither is established by the recovered callback table.

## Calibration and remaining support

The firmware refuses calibration writes at seconds 0 and 59. Otherwise it
writes register 6; the reader returns the low byte. Trim polarity, allowed
range and ppm/ppb units are unknown, so calibration should be preserved.

Linux integration needs coherent reads, a calendar-origin policy, shared
transaction/W1C support, and then clock-setting and alarm semantics. The RTC
class already supplies the public API; the missing work is the board contract,
not another userspace register interface. Current diagnostics cannot be used
as a substitute for those guarantees.
