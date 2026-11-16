# FM radio

The RM-443 receiver is the FM block of **N6000 BCM2048_B0**, normally controlled
at **I²C address `0x22`** with **GPIO61 / RAPS IRQ214** for events. Bluetooth
and FM share reset, clocks and companion startup. The port has no Linux FM
driver or validated reception/audio path.

The following contract comes from the schematic and MCUSW 10.12/08.90.
Tuning, physical IRQ behavior and analog routing still need hardware
confirmation. See [Bluetooth](bluetooth.md) for companion ownership and
[audio](audio.md) for I²C and AVILMA support gaps.

## Board connections

| Function | Connection |
| --- | --- |
| Control | BCM I2C_CLK/I2C_DA on the board I²C bus |
| FM interrupt | Active-low `FM_INTX`, GPIO61, IRQ214 |
| Shared reset/enable | `BT_RESETX`, GPIO64 |
| Clocks | Reference clock to XTALP; sleep clock to LPOIN |
| Analog right / left | BCM AUDIOR/AUDIOL through coupling capacitors to AVILMA Mic3PR/Mic3P |
| Analog returns | FM_AUDIO_NR/NL to AVILMA Mic3NR/Mic3N |
| Antenna | FMANT to BCM FMRF_INN |

The headphone lines couple through 15 pF capacitors and a 100 nH inductor to
FM_ANT. Selecting speaker output does not remove the headset-associated RF
path. The recovered register setup supports an analog audio path; a digital
FM capture interface is not established.

Both firmware images contain drivers for other radio chips. The negotiated
companion type **4** selects BCM byte registers; type 5 selects CSR. Those
initialization sequences are not interchangeable.

## I²C register transfers

Ordinary writes send register/value bytes; ordinary reads send a one-byte
register address and receive one byte. Register **`0x80`** instead drains
**150 bytes** from the RDS FIFO. Linux uses seven-bit address `0x22`, not the
wire address bytes `44`/`45`.

Firmware field tokens use `(register << 8) | mask` with right-justified values.
Register 0 is written directly; other fields use read-modify-write. The
10.12 wrappers add separate read/write resource reservations absent in 08.90.
Linux needs its own serialized, bounded transfers; S40 callback waits are not
a reusable timeout contract.

## Companion startup and the type `08` I²C gate

The first FM power write temporarily acquires the shared companion with claim
2. In 10.12, radio callback setup also controls these internal H4+ templates:

```text
08 00 15 fc 03 29 00 a2
08 00 15 fc 03 29 00 7f
```

The reserved byte after `08` is removed on the UART and an alignment byte is
added: the first packet becomes `08 15 fc 03 29 00 a2 00`. It carries vendor
command `FC15`, register `0029`, and one value byte.

The callback flow selects `A2` while enabling the radio client and `7F` when
removing it. The historical
[BCM2048 driver](https://github.com/torvalds/linux/blob/v5.3/drivers/staging/media/bcm2048/radio-bcm2048.c)
identifies register `29` as slave configuration, with enable bit `80` and
address mask `3F`. Together these support the interpretation that **`A2`
enables FM I²C at `22`**. The UART-to-I²C mapping is still an inference without
physical readback. The exact templates are absent in 08.90; its gate setup
may use a different path or controller defaults.

## Receiver registers

| Register | Recovered use |
| --- | --- |
| `00` | Bit 0 FM enable, bit 1 RDS enable |
| `01` | Band bit 0; stereo/blending fields in bits 1..3 |
| `02` | RDS FIFO control; startup writes 2 |
| `05` | RF mute bit 0, manual mute bit 1, DAC outputs bits 2/3, deemphasis bit 6 |
| `06` | Startup writes `1E`; full meaning unresolved |
| `07` | Seek threshold bits 0..6, direction bit 7 |
| `09` | Stop 0, preset 1, seek 2, AF jump 3 |
| `0A` / `0B` | Tuning frequency, low/high byte |
| `0C` / `0D` | Alternative frequency, low/high byte |
| `0E` / `0F` | Carrier error / RSSI |
| `10` / `11` | Event masks, low/high byte |
| `12` / `13` | Status flags, low/high byte |
| `14` | RDS FIFO waterline; startup writes 50 |
| `1A`..`1D` | PI match and mask bytes |
| `29` | Slave configuration; masked readback must equal `22` |
| `80` | RDS FIFO |

Startup selects mode 1 (FM) or 3 (FM+RDS), requests a 100,000-unit timer delay,
sets `reg01 & 1F = 6`, writes `reg06 = 30`, `reg02 = 2`, then checks
`reg29 & 7F == 22`. The timer argument is consistent with the microsecond
service but physical settling remains unmeasured. RDS setup writes waterline
50 and enables its event. Power-down masks the GPIO path, writes `reg05 = 3`
to mute/disable outputs, then clears FM/RDS control bits.

## Tuning and RDS

Frequency encoding is:

```text
encoded = frequency_khz - 64000
reg0a = encoded & 0xff
reg0b = encoded >> 8
```

For 100 MHz this is `8CA0`. Encoded `FFFF` is invalid. The recovered European
server policy uses **87.5–108 MHz** with 50 kHz manual steps. The HAL's broader
70–120 MHz assertion bounds do not establish the usable receiver band.

Preset tuning stops search, reads status, configures event masks and starts
mode 1. It polls every 1000 µs for at most 500 iterations, then allows 16000 µs
settling after completion. Low status bits mean:

| Bits | Meaning |
| --- | --- |
| `00` | Pending |
| `01` | Complete |
| `10` | Failure/band limit |
| `11` | Completion with failure/limit flag |

RSSI is an uncalibrated byte at `0F`; the S40 display clamps below `90` and
above `AF`, scaling the middle in two-count steps. Seek thresholds use
98, 95, 92 or 87. These values do not establish a dBm interface.

Event masks include `0001`/`0002` for tune results, `0004` low RSSI, `1000`
RDS sync loss and `0200` FIFO waterline. Device flag handling and GPIO latch
acknowledgement are separate; register read/clear timing remains unresolved.

The FIFO holds **50 three-byte records** per read:

```text
byte 0: block type in bits 6:4; error class in bits 3:2
byte 1: data high byte
byte 2: data low byte
```

A future V4L2 interface must translate block/error metadata and preserve FIFO
ordering across retuning and overflow. Station-name/RadioText/AF decoding can
remain in userspace.

## Audio and Linux support gaps

The analog route is BCM FM DAC → AVILMA Mic3 inputs → mixer/output route →
speaker or TPA6130A2 headphones. AVILMA gain, bias and routing are incomplete;
successful tuning alone cannot establish audible output.

Linux needs the RAPS I²C adapter, GPIO IRQ support, shared Bluetooth/FM startup
ownership and a current V4L2 driver. The historical staging driver is absent
from this Linux tree and is only a register reference. Shared reset must not
interrupt Bluetooth while FM closes or recovers.

V4L2 `CAP_LOW` frequency units are 62.5 Hz, so `frequency_khz * 16` gives the
userspace value. This is separate from the chip's `khz - 64000` encoding.
Regional policy, physical gate readback, status-clearing timing and analog
output validation remain the main implementation gaps.
