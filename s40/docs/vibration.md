# Vibration motor

The RM-443 motor is connected to **AVILMA VibraP/VibraN**, but the recovered
ARM code controls it through the DSP audio service. Linux has no vibration
driver. The DSP-to-AVILMA register sequence and physical drive characteristics
are still unknown.

MCUSW 10.12 and 08.90 use the same motor protocol, drive constants and phase
timing described here. These are firmware control values, not measured motor
voltage, PWM duty or mechanical response.

## Hardware and service path

AVILMA ball A2 (`VibraP`) connects through L2159 to M2101 positive; A1
(`VibraN`) connects through L2160 to negative. C2103, 10 nF, is across the
motor pair. The filters' `220R/100 MHz` markings do not describe DC resistance.

S40 requests pass through accessory resource **`0x1c`, object `0x34`**, then
DSP resource **`0xf3`**. Both motor terminals have dedicated outputs; there
is no recovered standalone GPIO or direct CBUS enable sequence.

The accessory API has four-byte requests beginning with transaction and ID:

| ID | Byte 2 | Reply ID |
| --- | --- | --- |
| `0x0c` | On/off | `0x0d` |
| `0x0e` | Default intensity, 0..100 | `0x0f` |
| `0x7c` | Inhibit; value 1 requests stop | `0x7d` |

The initial default intensity is **50**. Setting the default does not rewrite
a running command. The gate substitutes that default for zero intensity and
rejects another start while active; these S40 policy rules are distinct from
a Linux zero-magnitude stop request.

## DSP command

Motor commands use message **`0x24`**, with this 12-byte payload:

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 1 | Phase marker: 1 startup, 2 settling, otherwise 0 |
| 1 | 1 | `0x24` |
| 2 | 2 | Big-endian subblock count, 1 |
| 4 | 2 | Big-endian subblock ID, `0x000e` |
| 6 | 2 | Big-endian subblock length, 8 |
| 8 | 2 | Big-endian enable, 0 or 1 |
| 10 | 2 | Big-endian signed 16-bit drive |

Disable sends enable=0 and drive=0. Enabled-zero is a separate settling
command. Negative drive uses two's complement. Calculated examples:

```text
Startup:       01 24 00 01 00 0e 00 08 00 01 7f ff
Sustain 50%:   00 24 00 01 00 0e 00 08 00 01 3b ea
Reverse brake: 00 24 00 01 00 0e 00 08 00 01 80 01
Settling:      02 24 00 01 00 0e 00 08 00 01 00 00
Disabled:      00 24 00 01 00 0e 00 08 00 00 00 00
```

The reviewed ARM path recognizes reply `0xf325` but does not parse a motor
status or wait for it before advancing phases. The reply format and physical
completion semantics remain unresolved. Shared transport ownership is covered
in [audio](audio.md).

## Drive conversion

For signed intensity `p` in -100..100, the recovered conversions are:

```text
startup/brake = trunc(p * 327.67)
sustain(0) = 0
sustain(p) = trunc(trunc((p / 200.0 + sign(p) * 0.75) * 32767.0)
                   * 0.46810699588477367)
```

`trunc` means toward zero. The intermediate truncation matters. Firmware also
handles narrowed-result sign overflow; a Linux implementation should bound
its input before conversion and use integer arithmetic.

| Intensity | Sustained drive | Startup interval |
| --- | --- | --- |
| 1% | 11580 | 20 ms |
| 25% | 13421 | 30 ms |
| 50% | 15338 | 40 ms |
| 75% | 17255 | 50 ms |
| 100% | 19172 | 60 ms |

The nonzero offset provides a substantial minimum command. Sustained 100%
therefore uses 19172; the full signed endpoint is reserved for startup/braking.

## Phase timing

| State | Behavior |
| --- | --- |
| 0, idle | Start enters startup |
| 1, sustain | Stop enters reverse brake |
| 2, enabled-zero settling | Start cancels settling and enters startup |
| 3, startup | Further starts ignored; stop cancels startup and adjusts braking |
| 4, reverse brake | Start cancels braking and enters startup |

Timers use 5 ms ticks. Startup drives full strength with the requested sign:

```text
startup_ticks = trunc((abs(p) / 100.0 + 0.5) * 40.0) / 5
```

After startup, the motor changes to sustained drive. A normal stop drives full
strength with the opposite sign for:

```text
brake_ticks = trunc(abs(p) / 100.0 * 1.33 * 30.0) / 5
```

This is 15 ms at 50% and 35 ms at 100%. Stopping during startup adjusts the
brake duration using the timer's returned remaining count; its exact expiry
boundary behavior is unresolved. The unsigned conversion clamps negative
results to zero, and the timer enforces a minimum of one tick.

Brake expiry sends enabled-zero for **1500 ms**, then disable. A separate
immediate-disable operation bypasses these phases, but its interaction with
outstanding timers and DSP completion still needs to be established.

## Linux support requirements

The recovered path requires a shared DSP provider before a vibration driver
can operate. A direct AVILMA implementation would first need the missing
register, clock and output-enable contract. One motor can be exposed through
`FF_RUMBLE`; transport work must run outside the atomic effect callback.

The implementation must cancel old phase work on stop, close, suspend or DSP
restart and must not replay an old effect. Hardware confirmation is needed
for drive limits, braking and disable completion, especially while audio also
owns the DSP. No current emulator or Linux result establishes those properties.
