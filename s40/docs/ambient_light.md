# Ambient-light sensing on RM-443

The Nokia 6303 classic has a **TSL2563CL at I²C address `0x29`**. The Linux
port has no sensor driver or automatic-brightness service yet. The hardware
contract below comes from the board schematic and MCUSW 10.12/08.90; optical
calibration and physical bus timing remain unverified.

## Sensor and bus

| Connection | Board wiring |
| --- | --- |
| Part | N2460, TSL2563CL |
| VDD | Shared VAUX rail, with 100 nF decoupling |
| Address select | Grounded; seven-bit address `0x29` |
| SCL / SDA | PUSL[0] / PUSL[1] |
| INT | Unconnected in the schematic; use polling |

VAUX also supplies the display connector. Power down the sensor through its
control register rather than removing the shared rail. LP5521 lighting uses
the same bus; see [lighting](lighting.md).

S40 selects RAPS I²C controller 1, physical base `0x20032200`, with timing
values `SCLL = SCLH = 55`. The headphone path uses different timing. See
[audio](audio.md) for controller ownership and clock requirements.

## Acquisition

Sensor words are **little-endian**. The recovered commands are:

| Operation | Command/data |
| --- | --- |
| Power on / down | Write `80 03` / `80 00` |
| Read control | `80`, one byte |
| Read or write timing/gain | `81`, one byte |
| Read chip ID | `8A`, one byte |
| Broadband CH0 | `AC`, two bytes |
| Infrared CH1 | `AE`, two bytes |

`AC`/`AE` include the command and word-protocol bits. The sensor's
[register documentation](https://www.mouser.com/datasheet/2/588/TSL2562_63_DS000421_1_00-2324560.pdf)
describes the same low-byte-first word transfers.

S40 configures timing, powers on, waits for conversion, reads CH0, adjusts the
range if necessary, then reads CH1 and powers down. Its rounded conversion
waits and gain modes are:

| Timing byte | Gain | Wait |
| --- | --- | --- |
| `00` / `10` | 1× / 16× | 15 ms |
| `01` / `11` | 1× / 16× | 110 ms |
| `02` / `12` | 1× / 16× | 430 ms |

The ordinary automatic-range path uses `01`, `02` and `12`. At `01`, CH0
4096..37143 is accepted; lower counts request more sensitivity and higher
counts select less. At `02`, the accepted range is 2049..65534. At `12`,
65535 requests less sensitivity. Bright overflow from `01` selects `00`
and marks the result with sentinel **65536**. That sentinel is not a lux
measurement. Periodic subscriptions use a 500 ms sampling cadence.

## Calibration and conversion

PMM record **356, index 1** contains eight bytes. Offsets 0 and 2 are unsigned
16-bit CH0/CH1 coefficients in Q13; offset 6 equal to 1 marks calibration
valid. The remaining fields are unresolved. Per-handset coefficients are not
included in the port.

S40 multiplies each channel by its coefficient divided by 8192, normalizes
15 ms readings by approximately 29.343065 and 110 ms readings by 3.980198,
and multiplies 1× readings by 16. Let the resulting channels be `C0`, `C1`,
with `r = C1/C0`:

| Ratio | Illuminance calculation |
| --- | --- |
| Up to about 0.52 | `0.0315*C0 - 0.0593*C0*pow(r, 1.4)` |
| About 0.52..0.65 | `0.0229*C0 - 0.0291*C1` |
| About 0.65..0.80 | `0.0157*C0 - 0.0180*C1` |
| About 0.80..1.30 | `0.00338*C0 - 0.00260*C1` |
| Above about 1.30 | Zero |

The routine clamps negative results. Zero CH0 takes a separate zero-result
path; zero CH1 is outside the positive-ratio branch. Bright overflow overrides
the ordinary result with 65536.

The fitted part is labeled **CL** in the schematic, while the recovered
constants match the manufacturer's **CS-package** formula. The actual part,
cover compensation and calibration need checking before these numbers can be
presented as accurate lux.

## Filtering and brightness policy

S40 uses a seven-sample median. After a delayed update it inserts the newest
sample `clamp(elapsed_ms / 500, 1, 7)` times to age out old history. Category
changes use hysteresis:

| Current category | Downward transition | Upward transition |
| --- | --- | --- |
| 3 | — | Above 25 lux → 4 |
| 4 | Below 18 → 3 | Above 75 → 5 |
| 5 | Below 40 → 4 | Above 750 → 6 |
| 6 | Below 300 → 5 | Above 4000 → 7 |
| 7 | Below 1200 → 6 | — |

A valid previous category changes by at most one step per update. Without
calibrated-category enable, the service returns category 2. These categories
are policy inputs, not PWM values; the backlight mapping is separate.

## Linux integration gaps

The tree contains [`tsl2563.c`](../../drivers/iio/light/tsl2563.c) and its
[device-tree binding](../../Documentation/devicetree/bindings/iio/light/amstaos,tsl2563.yaml).
It supports a polling configuration, but the port still needs the RAPS I²C
adapter and shared supply ownership. The driver has no regulator consumer.

Before using processed readings, account for these differences:

- Linux uses Q10 calibration internally; Nokia's PMM coefficients are Q13.
- Linux SMBus word reads use `8C`/`8E`; S40 sends `AC`/`AE`. Confirm the
  required transaction format with the adapter.
- On ARM926, `ch1 << 14` in `tsl2563_adc_to_lux()` uses a 32-bit
  `unsigned long` and can overflow for normalized readings. The shift needs
  wide arithmetic before relying on the processed value.
- The upstream gain table, normalization and error handling differ from S40.
  Physical raw-channel and reference-light measurements are still needed.

Automatic brightness belongs in userspace once sensor and backlight controls
exist. Keep manual brightness available and retain a usable level on sensor
failure.
