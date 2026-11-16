# Lighting and LCD initialization

RM-443 has a Betty-controlled display boost supply, a separate keypad gate,
and an LP5521 driving two white LEDs. Some panel variants also accept
brightness commands. Linux currently inherits the powered panel and bus
timing from S40; it has no backlight/LED class devices or cold panel power
sequence. Current display controls are documented in [display](display.md).

The wiring and register contracts below come from the board schematic and
MCUSW 10.12/08.90. Electrical brightness/current characteristics and complete
cold initialization remain unverified.

## Physical paths

| Path | Board connection |
| --- | --- |
| Display LEDs | Betty `LedOut` drives BD6074GUT enable; boost output reaches `VLED_pos` and display connector LED input |
| Main keypad | Two LED strings share `VLED_pos`; `GENOUT1` controls their return switches |
| Additional white LEDs | LP5521 N2491 G/B outputs drive V2512/V2513; red is unconnected |
| LP5521 supply/control | VBAT_BB supply, VIO enable, grounded address pins, PUSL I²C, connected `CLK_32K` |
| Panel logic | VIO and VAUX, GPIO55 reset, eight-bit parallel bus and tear signal |

The main keypad depends on the LCD's boost supply. LP5521 uses seven-bit
address **`0x32`**. VAUX and PUSL are also used by the
[ambient-light sensor](ambient_light.md).

## Betty brightness and keypad gate

These are **CBUS chip 2** fields:

| Register / mask | Recovered operation |
| --- | --- |
| `0x05 / 0x007f` | Seven-bit LED/PWM code, 0..127 |
| `0x09 / 0x0001` | Enable when PWM code is nonzero on mapped PMIC family 13 |
| `0x09 / 0x0080` | Keypad `GENOUT1`, zero off / one on |

S40 converts brightness 0..10000 to `floor(max_code * brightness / 10000)`.
The fitted Betty family maps to max code 127; older compatibility branches use
other limits. Preserve PMIC identity/revision handling rather than applying a
whole-register value. The keypad path is binary, with no recovered independent
PWM control.

The two consumers need shared boost ownership: keeping the keypad gate on
while disabling its supply cannot provide independent keypad illumination.
The effect on display illumination while keeping that supply active still
needs measurement.

## LP5521 white LED channels

| Function | Registers |
| --- | --- |
| Enable / execution | `0x00` |
| Engine/direct mode | `0x01` |
| R/G/B PWM | `0x02` / `0x03` / `0x04` |
| R/G/B current | `0x05` / `0x06` / `0x07` |
| Configuration | `0x08` |
| Status / reset | `0x0c` / `0x0d` |
| Engine program memory | `0x10` / `0x30` / `0x50` |

S40 enters direct mode, sets current, then writes
`floor(255 * brightness / 10000)`. The board current code is **`0x32`**, or
5 mA using the chip's 0.1 mA steps. Its reset shadow `0xaf` is not the board
current setting. See the [LP5521 datasheet](https://www.ti.com/lit/ds/symlink/lp5521.pdf).

The tree contains `drivers/leds/leds-lp5521.c` with compatible
`national,lp5521`; child channels 1/2 select G/B. It resets existing engines
and PWM state. This version has no regulator consumer, and its `32k_clk`
lookup falls back to the internal oscillator if unavailable or unsuitable.
Supply and clock ownership still need board integration; the connected clock
wire alone does not configure the driver.

## Panel rail and reset

The recovered panel resource performs these **AVILMA chip 1** operations:

| Operation | Register / mask / value |
| --- | --- |
| Initialize | `0x10 / 0x1000 / 1` |
| On | `0x10 / 0x4000 / 1` |
| Off | `0x10 / 0x4000 / 0` |

The electrical names and shared consumers of these fields are not fully
established. Firmware's generic resource initializer also touches unrelated
resources and cannot be replayed as a panel driver.

Cold reset sets **GPIO55** to output, then drives low 10 ms, high 10 ms,
low 10 ms, high, and waits 140 ms before probing. A separate recovery path
uses 5 ms pulses and 120 ms waits. Reset requires pinmux/direction ownership
as well as output writes.

## Panel identification and command programs

S40 reads DCS commands **`DA`/`DB`/`DC`** for three identity bytes. It rejects
zero, `FF` and echoed command bytes. The labels `SHARP` and `SEIKO` do not
identify an exact controller part.

Program-selection records contain three 16-bit identity values, a reserved
halfword and two program pointers. `00FF` is a wildcard; first match wins.
Both firmware versions contain multiple panel variants, with additional
programs in 10.12. A single recovered sequence is therefore not a universal
board initialization.

The read transport differs: 10.12 writes fixed MODE `0x0bb40000`, whereas
08.90 derives MODE from bus configuration. Both discard an initial FIFO read.
Four-byte status reads then consume four bytes in big-endian order. Physical
read timing and dummy cycles remain unresolved.

Command programs are **little-endian 16-bit words** terminated by `FFFF`:

| Encoding | Operation |
| --- | --- |
| `20cc` | Send command byte `cc` |
| `40dd` | Send data byte `dd` |
| `80tt` | Wait `tt` ms |
| `0200` | Send current orientation |
| `0100` | Send selected panel brightness |
| `0800` | Send stored display-control/CABC value |
| `8800` | Send `0F` and read a status byte |
| `1000` | Set full-screen window and transfer initial framebuffer |
| `4400` | Sharp-path `09` status read/check |
| `0400` | Refresh partial/normal-mode handling |
| `2200` | Conditional tear-enable `35`, data zero |

One recovered power program sends sleep-out `11`, waits 120 ms, reads status,
sets gamma/orientation/pixel format, initializes the framebuffer, then sends
display-on `29`. Sleep sends display-off `28`, waits 95 ms, sends sleep-in
`10`, then waits 50 ms. Other panel programs differ.

Linux must substitute its own framebuffer and restore its selected pixel
format after a power program. Full cold bus timing/clock setup is still
missing; the current transport derives `bus_mode` from inherited MODE state.

## Panel-side brightness

The panel brightness path accepts 0..100 and sentinel 255 for the default.
ID3 `56`/`60` uses command `6D` with steps 0..6; IDs `6F`/`7F` use `51` scaled
to 0..255. The 10.12 `73` branch uses `6D` with thresholds 15, 30, 43, 60, 80,
100. This path is separate from Betty PWM and is not selected by the board's
ordinary lighting channel table.

## Linux support gaps

Backlight/keypad controls need a common Betty owner and coordinated boost
references. LP5521 needs the RAPS I²C adapter and board clock/supply setup.
Cold panel support additionally needs validated ID reads, variant selection,
logic-rail control, reset and bus initialization. Brightness and panel waits
belong in sleepable operations; CBUS transfer locks must remain short so
watchdog service can continue. See [power](power.md) for PMIC ownership.
