# Battery monitoring and charging

The Linux port has no battery `power_supply` device or managed charger.
The following register and conversion details were recovered from RM-443
10.12/08.90 and the board schematic. Calibrated monitoring is feasible, but
the charging control loop, fault handling and physical measurement units
remain incomplete.

## Board and PMIC access

- X2070 supplies battery positive, ground and **BSI**. BSI reaches AVILMA
  pin L11 and identifies the pack.
- R2071 is a separate **47 kΩ board thermistor** on AVILMA BTemp, pin E9.
  Its temperature is not automatically the battery-pack temperature.
- The dedicated charger input X2000 reaches Betty `VCharIn`; `VCharOut`
  feeds `VBAT_BB`. USB VBUS is a separate net.
- R2300 connects to Betty SenseP/SenseM. Current gain and offset still need
  physical calibration.

AVILMA is **CBUS chip 1**, Betty **chip 2**. The existing access implementation
is `kernel/overlay/arch/arm/mach-nokia-raps/rm443_power.c`; see [power](power.md).
It serializes individual transfers. Battery sampling needs ownership of the
entire multi-transfer operation as well.

Firmware field tokens encode `(register << 16) | mask`, with right-justified
values. Some event registers use direct writes rather than ordinary
read-modify-write. Shared Betty registers also serve USB and lighting, so
whole-register replay is unsuitable for a battery driver.

## ADC selection and sampling

AVILMA register **`0x08`** holds the selector in bits 13:10 and a 10-bit result
in bits 9:0. The recovered channel map is:

| Logical measurement | HAL ID | ADC channel | Meaning |
| --- | --- | --- | --- |
| 1 | 11 | 8 | Battery voltage |
| 2 | 9 | 3 | Charger-input voltage |
| 3 | 10 | 9 | Unresolved |
| 4 | 6 | 1 | BSI |
| 5 | 1 | 2 | Board thermistor |
| 6 / 7 / 8 | 5 / 13 / 4 | 4 / 5 / 10 | Unresolved |
| 19 | 14 | 12 | Unresolved |
| 20 | 21 | 11 | Second temperature-conversion path; physical input unresolved |
| 21 / 22 / 23 | 19 / 20 / 22 | 6 / 7 / 13 | Unresolved |

Logical 14 uses Betty's current path instead of the AVILMA ADC.

A single conversion acquires **hardware semaphore 1**, adjusts repetitive
sampling if needed, writes the selector, delays, reads the result, restores
selector **8**, then releases ownership. The recovered delay argument is 3;
its physical conversion timing is not yet established. Linux needs to
serialize select/wait/read/restore while preserving repetitive-sampling state.
The per-transfer CBUS lock does not provide that contract.

Repetitive sampling uses register `0x09`. The rate helper maps values below
122 to zero, values through 15616 to `value / 61 - 1`, and larger values to
255. These native timing units have not been established as a sampling rate.

## Calibration

The common default conversion is:

```text
x = max(0, common_offset + raw * common_gain / 10000)
common_offset = 0
common_gain = 13196
```

Channel conversions follow that stage. Native outputs are integral values
clipped to their permitted signed 16-bit range.

| Measurement | Default conversion |
| --- | --- |
| VBAT | `2581 + 21357*x/10000`, truncated; millivolts |
| Charger input | `129800*x/10000`, truncated |
| BSI | `1177 / (2500/x - 1)`, with endpoint handling |
| Board temperature | Thermistor conversion below; integral kelvin |

For example, raw VBAT 512 produces 4023 mV with defaults. Raw BSI 778 produces
820 native units, consistent with 82 kΩ at 100 Ω per unit. These are calculated
examples, not handset measurements.

The thermistor defaults are 2127, 4000 and 298:

```text
xmax = common_offset + common_gain * 1023 / 10000
ratio = 2.127 / (xmax / x - 1)
temperature_K = trunc(1 / (1 / 298 + ln(ratio) / 4000))
```

This applies inside the valid ADC range. Endpoints and failed conversions
need explicit invalid-sample handling.

Factory record family **309** overrides these defaults. Subrecord 3 contains
12-byte calibration records; common type 5 with channel marker `0xfe`
overrides global offset/gain. The state also has an XOR-`0xaa` duplicate.
A Linux handoff must copy validated coefficients or records before reclaiming
S40 memory. A native PMM reader is not implemented.

## Presence, identity and gauge

AVILMA register **`0x16`, bit `0x0100`** reports battery presence. S40 confirms
presence around repeated BSI sampling, taking the maximum of ten median-of-five
readings in the normal path.

BSI **452..1399** selects battery type 4. Its capacity parameter is:

```text
trunc((4338 * BSI - 1800000) / (BSI + 1000))
```

BSI 820 gives 965. This parameter is neither remaining charge nor a percentage.
The port has no validated fuel-gauge model, so capacity percentage cannot be
derived from voltage alone.

The firmware boot gates use 3300 mV normally and 3400 mV on the relevant
charger path. Version 10.12 also has a special-mode branch above 3000 mV.
These are boot thresholds, not charge termination or cell-protection settings.

## Betty current measurement

Betty **`0x0c & 0x00ff`** supplies an interval/divisor and **`0x0d`** a signed
16-bit accumulator. Zero divisor is treated as one; signed endpoints `0x7fff`
and `0x8000` produce an invalid sentinel. The ordinary calculation divides
the integer accumulator by the divisor, multiplies by **0.803094161**,
truncates and negates, in that order.

Physical scale, sign and averaging interval remain unverified. The interval
setter rounds upward in 250-unit steps and saturates at 255; zero writes zero
but returns 250. Current sampling is enabled by `0x08/0x0040`, and updating
the interval pulses `0x08/0x0080` high then low.

The sampling state machine differs between firmware versions: 10.12 ignores
zero together with sentinels `0x7fff`, `0x7ffe`, `0x7ffd`; 08.90 processes zero.
The hardware validity rule must be resolved before exposing current readings.

## Charger control

AVILMA **`0x16/0x1000`** reports charger presence. Its event is 14, pending
bit **`0x0002`** under the firmware mapping `0x8000 >> event`. Betty events
5 and 6 also enter charging callbacks; their physical fault meanings remain
unresolved. The current Linux power setup masks AVILMA events and has no PMIC
child IRQ infrastructure.

| Betty register/mask | Recovered use |
| --- | --- |
| `0x04/0x00ff` | Eight-bit charge-drive value; not a current in mA |
| `0x08/0x0004` and `0x08/0x0008` | Cleared for nonzero drive; set for zero drive |
| `0x08/0x0001` | Separate charge-manager mode |
| `0x08/0x0040` | Current-measurement enable |
| `0x08/0x0080` | Current-measurement update/reset pulse |

The zero-drive sequence writes register 4 low byte to zero, then sets
register 8 bit 2, then bit 3. Nonzero drive writes the duty before clearing
those bits. This is only part of the charger state machine.

Recovered control-loop details include temperature transitions below 273 K
and above 333 K in two states; smaller drive adjustments below 283 K; repeated
low-current checks before termination; and a voltage target adjusted from
4195 toward 4150 by a current-derived value. The complete state meanings,
hysteresis, input limits, precharge/recharge policy and timeouts are still
missing. These fragments do not define a usable Linux charging algorithm.

## Linux support requirements

Read-only monitoring needs a serialized ADC service, per-device calibration
and explicit sample validity. VBAT can then become `VOLTAGE_NOW` in µV.
The board thermistor belongs in IIO/thermal until its relation to pack
temperature is established. Current needs validated units and averaging
semantics before selecting `CURRENT_NOW` or `CURRENT_AVG`.

Managed charging additionally requires shared PMIC IRQ ownership, the complete
Betty control loop and fault response, and a defined takeover from S40. The
present handoff does not establish whether physical charging stops or
continues correctly after the S40 manager exits. See the
[power-supply class contract](../../Documentation/power/power_supply_class.rst)
for Linux units and property meanings.
