# Bluetooth on the Nokia 6303 classic

Bluetooth is not implemented in this Linux port. The BCM2048_B0 companion uses
Nokia H4+ over a RAPS serial peripheral, circular receive DMA, GPIO flow control
and a downloaded controller patch. The following contracts were recovered from
08.90/10.12 firmware and board wiring; physical radio operation under Linux is
unverified.

Register addresses are physical. Firmware `F0...`/`F1...` addresses identify
inspected code/data, not callable Linux interfaces; S40 RAM pointers do not
survive takeover. See [hardware](hardware.md) for mapping conventions.

## Board wiring and ownership

The service schematic connects N6000 BCM2048_B0 to RAPS as follows.
Directions are from the host perspective.

| Signal | RAPS GPIO | Direction | Meaning |
| --- | ---: | --- | --- |
| `BTRSTX` / `BT_RESETX` | 64 | Out | Active-low controller reset; also connects to BCM `VREG_CTL` |
| `BTWAKE` / `BT_WAKEUP` | 29 | Out | Host wakes the Bluetooth companion |
| `BTHOSTWAKE` / `UART_WAKE` | 67 | In | Companion requests host serial activity |
| `BTH_CLK_REQ` | 53 | In / alternate function | Companion clock request; distinct from HOSTWAKE |
| `BTCTS` | 69 | In | Host transmit permission; physical low is asserted |
| `BTRTS` | 70 | Out | Host receive permission; physical low is asserted |
| `BTDAIN` | 59 | In / serial function | Data from BCM `UART_TXD` |
| `BTDAOUT` | 60 | Out / serial function | Data to BCM `UART_RXD` |

The host's RTS connects to the controller's CTS input.

The companion has `SYSCLK` from `REFCLK_I` and a separate `SLEEPCLK` input.
The negotiation advertises a **38.4 MHz controller reference**; this must not
be confused with the **112 MHz nominal RAPS serial parent** calculated below.
The exact clock-request routing, reference gating and sleep-clock control
still need a Linux clock/power implementation.

### GPIO and pin multiplexing

10.12 `F01E4CD0` maps selectors 0..4 to RTS70, WAKE29, RESET64, HOSTWAKE67,
and CTS69. `F01E5346` initializes their direction/pinmux; `F01E4F74` switches
CTS between GPIO edge monitoring and serial flow control. GPIO input/output
tables at `F110D4FC` resolve these physical register banks:

| GPIO group | Output set | Output clear | Input | Relevant bits |
| --- | --- | --- | --- | --- |
| 0..31 | `20031cb4` | `20031cb8` | `20031d48` | WAKE29 = bit29 |
| 32..63 | `20031cc4` | `20031cc8` | `20031d88` | CLK_REQ53 = bit21; RX59/TX60 = bits27/28 |
| 64..78 | `20031cd4` | `20031cd8` | `20031dc8` | RESET64 = bit0; HOSTWAKE67 = bit3; CTS69 = bit5; RTS70 = bit6 |

These are 32-bit set/clear operations, not a read-modify-write of an output
value register. Direction, pull configuration and pin selection are separate.

Recovered pinmux operations in `F01E5346` include clearing `0x03000000` at
`20031c48` and setting `0x01000000` at `20031c44`. Serial mode 3 clears
`0x3000` at `20031c58` and sets `0x1000` at `20031c54`; mode 2 selects GPIO
CTS/RTS instead. CTS interrupt arming temporarily clears `0x0c00` at
`20031c58`; returning CTS to the serial function sets `0x0400` at
`20031c54`. Preserve unrelated pin fields and serialize access with other
board drivers.

The firmware GPIO-to-INTC table routes **CTS69 to IRQ207**, **HOSTWAKE67 to
IRQ215**, and **CLK_REQ53 to IRQ213**. GPIO peripheral edge status must be
acknowledged separately from masking/unmasking the RAPS interrupt source.
The port has an INTC domain but no general GPIO/pinctrl/IRQ provider.

## RAPS serial register contract

The receive bank starts at **`0x20020000`**, transmit at **`0x20020100`**.
Configuration also clears `20020208`, `20020308`, and `2003051c`.
The last register is outside the serial aperture.

| Physical register | Observed firmware operation | Confidence / limitation |
| --- | --- | --- |
| `20020004` | RX disable=0, enable=1 | Explicit in init and direction control |
| `20020104` | TX disable=0; enable writes selected mode, initially 2 | Mode is not a generic Boolean |
| `20020008`, `2002000c`, `20020060` | RX integer, next integer, fractional timing values | Calculation recovered; field names descriptive |
| `20020108`, `2002010c`, `20020138` | Corresponding TX timing values | Same qualification |
| `20020010`, `2002005c` | 10.12 programs 90% and 20% of RX timing value | Sampling/filter interpretation unconfirmed; 08.90 setter omits these writes |
| `20020014/18/1c` | RX format arguments | Startup supplies 7, 0, 0 |
| `20020110/14/18` | TX format arguments | Same startup arguments; do not import 16550 bit definitions |
| `20020044`, `20020130` | Written zero by 10.12 init | Meaning unresolved |
| `20020038` | Writes 10 during setup, then 61 before RX enable | Exact bit definitions unresolved |
| `20020030` | Receive byte data port | DMA peripheral width 8 bits |
| `20020124` | Transmit byte data port | PIO loop uses 32-bit `STR` with one byte value per write |
| `20020134` | Nonzero means CTS asserted in serial mode | Firmware converts it back to physical GPIO polarity |

`F01E5234` supplies the initialization sequence and `F01E52C8` the PIO write
loop. `F01E5194` handles **IRQ88**: mask, send up to **31 bytes**, and re-enable
until the queued byte count is exhausted, then invoke the completion callback.
The physical FIFO capacity and final TX-drain condition remain unresolved.

Initialization masks IRQ86 and IRQ87. RX normally uses DMA, so neither a
PIO RX-ready flag nor IRQ86's precise receive/error conditions are established.

### Baud calculation

10.12 `F01E4EEC` reads `20032c80` bit12 and PLL0 controls `20032d20/24` to
derive the serial parent. The reference is 38.4 MHz when bit12 is set, else
19.2 MHz. In the PLL-enabled branch, multiplier fields are bits11:7 of each
PLL register, with the second field forming the high five bits; zero is
treated as one. The divisor is `((control >> 5) & 3) + 1`. The routine then
divides the PLL result by two. The bypass branch instead uses
`(control >> 2) & 3`; Linux must reject a zero divisor rather than trust
uninitialized controls. NOLO's recovered PLL0 configuration gives 224 MHz,
hence the nominal **112 MHz** serial parent.

08.90 `F01E5C00` obtains clock ID0 through `F037B378`, then halves it. The
transport clock comes from clock state in both builds, not an unexplained
fixed UART input constant.

For parent `F` and requested baud `B`, `F01E5092` computes:

```text
R = (F / B - 3) / 2
T = 2 * R + 2
RX +08 = floor(R)           RX +0c = floor(R + 1)
RX +60 = floor(frac(R) * 2^32)
TX +08 = floor(T)           TX +0c = floor(T + 1)
TX +38 = floor(frac(T) * 2^32)
10.12 also: RX +5c = floor(0.20 * R), RX +10 = floor(0.90 * R)
```

S40 performs the operations in single-precision software floating point.
`F0517C44 -> F0E1BA90` scales by powers of two: arguments -1, 1 and 32 mean
divide by two, multiply by two, and multiply by **2^32**, respectively.
The last operation is not multiplication by 32. Linux should implement this
with checked integer/fixed-point arithmetic and measure the resulting rate;
it must not use floating point in the kernel.

Calculated examples reproducing S40's float rounding with a 112 MHz parent:

| Requested baud | RX +08 / +0c / +60 | TX +08 / +0c / +38 |
| ---: | --- | --- |
| 120000 | 465 / 466 / `2aaa0000` | 932 / 933 / `55540000` |
| 921600 | 59 / 60 / `438e4000` | 120 / 121 / `871c8000` |
| 4000000 | 12 / 13 / `80000000` | 27 / 28 / `00000000` |

These are calculated register values, not measured baud rates. In particular,
the 38.4 MHz value in the H4+ packet describes the companion clock and must
not replace `F` in this calculation.

## Receive DMA

`F01E4E94` requests firmware logical peripheral **11** from `F037C228`.
Its descriptor at `F110DD98` contains RX VA `FFD20030`, TX VA `FFD20124`,
width **8 bits**, RX hardware request **0**, and TX hardware request **1**.
Logical endpoint 11 is not hardware DMA request 11: the latter is used by
other peripherals, including SD TX. Mixing these number spaces would route
the wrong device.

The circular engine has **four channels**, **IRQs 6..9**, and a **4-byte
channel stride per register array**. It is separate from the six linear
channels with `0x400` stride used by the LCD driver.

| Channel-0 physical address | Operation / meaning |
| --- | --- |
| `20040820` | Peripheral request selector; Bluetooth RX selects 0 |
| `20042440` | Run: 0 stopped, 2 receive, 1 transmit |
| `20042480` | Free elements in RX ring |
| `200424c0` | Return consumed elements to hardware |
| `20042500` | Control, initialized zero |
| `20042540` | Element width in bytes, 1 for Bluetooth |
| `20042580` | Idle timeout, common engine clock prescaled to 2 kHz |
| `200425c0` | Peripheral physical address, `20020030` |
| `20042600` | Ring physical base |
| `20042640` | Channel reset |
| `20042680` | Ring length in elements |
| `200426c0` | Producer index in elements |
| `20042700` | Free-space interrupt threshold |

Common initialization uses `20042404/08/0c`, a source-derived 2 kHz prescaler,
and `20042428`. The separate cache command at `20042418` is polled until
self-clearing. The complete initialization order and timeout behavior still
need validation on hardware.

S40 allocates a 4 KiB block split into four 1024-byte rings, translates their
addresses and arranges cache handling. Its Bluetooth ring uses one-byte
elements. `F037C560` calculates available data from producer minus software
consumer modulo 1024, treating free=0 as full; it copies bytes and returns
credits through `200424c0`. `F037C4CE(requested)` sets the threshold to
`ring_length - requested + 1`. Preserve idle/partial-packet notification;
waiting only for a large threshold would strand small HCI completion events.

## Reset and H4+ startup

10.12's transport startup at `F01E4580` initializes mode 2 and baud 120000,
asserts BT_WAKEUP, installs callbacks, and invokes `F01E496C`. Reset is driven
low for **6000 microseconds**. The firmware samples HOSTWAKE67 and CLK_REQ53
while reset is asserted, then releases RESET64. When the clock-request pin is
in its alternate mode, the code temporarily changes its mux to sample it and
restores the original selection.

The expected first event is **CTS69 falling**, delivered through IRQ207.
`F01E3D4E` posts event 3 to task 34; state 1 advances to state 2. The startup
deadline uses timer `0xb3`, argument 500, at 5 ms per OS unit: **2.5 seconds**,
not 500 ms. The diagnostic tuple checked at `F01E3C42` is
`{HOSTWAKE_before, HOSTWAKE_after, CLK_REQ_before, CLK_REQ_after} = {0,1,0,1}`.
For the non-sampled clock-request branch, the code supplies those expected
CLK_REQ values itself; that branch is not an electrical observation.

The main wire sequence is:

| Stage | Wire operation | State / purpose |
| --- | --- | --- |
| Reset ready | CTS low | Companion ready at initial 120000 baud |
| Negotiate | `06 0a 00 a1 01 00 00 4c 00 96 00 00` | Request byte-oriented H4+ and setup speed |
| Negotiation response | Type `06`, length 12, acknowledgement `20`; manufacturer/revision fields | Identify controller; exact physical revision byte still needs capture |
| Flow transition | Raise host RTS; wait for the next CTS-low event | Coordinate rate transition |
| Setup speed | Host changes to 921600 | Firmware state 4 |
| Alive | Request `07 02 55 00`; response `07 02 cc 00` | Verify the new link |
| Patch | `FC4C` writes, `FC4E` launch, board configuration | Firmware state 5 and BCM setup callback |
| Operational speed | `FC18` requests 4000000; host follows after completion | Both S40 builds |
| Identity | `FC01` with six handset address bytes | Outside the immutable patch command list |
| Further setup | Second alive, `FC00`, type `08` `FC15` radio-register operation | Seen in the archived 10.12 S40 emulator trace |

Negotiation fields are little endian. `a1 01` is 417, consistent with rounded
`38,400,000 * 10 / 921,600`; `00 96` is 38400 in kHz; `4c` selects byte
protocol. Firmware accepts more than one revision byte; an emulator-provided revision
does not identify the handset controller ROM.

### Packet framing

| Type | Packet after type byte | Use |
| --- | --- | --- |
| 01 | opcode (16 bits), parameter length (8 bits), parameters | Host HCI commands |
| 02 | handle/flags (16 bits), data length (16 bits), payload | ACL data |
| 03 | handle/flags (16 bits), data length (8 bits), payload | SCO framing; audio routing remains separate |
| 04 | event (8 bits), parameter length (8 bits), parameters | HCI events |
| 06 / 07 | length (8 bits), payload | Nokia negotiation / alive |
| 08 | Command-style header outbound; event-style header inbound | Dedicated companion radio channel |

Every wire packet is padded to an **even total byte length**, including the
type byte. Padding is not part of the HCI payload length. RX must retain its
pending padding state across separate DMA deliveries.

S40's internal packet adds a reserved byte immediately after the type:
`type, reserved, ...`. `F01E3CB4` removes that byte, then pads the wire packet.
For example, a completion is `04 0e 04 01 <opcode-lo> <opcode-hi> 00 00` on
the wire, with the last zero being padding. Passing the internal image bytes
straight to Linux or the UART corrupts framing.

The type `08` request seen after setup is
`08 15 fc 03 29 00 a2 00`, or value `7f` in the alternate callback case. The
modeled response carries status 0. `F01E62AC` checks the type, event `0e`,
length 6, command credit 1, opcode `FC15` and register `0029`, but does **not**
check the status byte. The firmware then waits ten OS units (50 ms) before
reporting readiness. A Linux implementation should validate status as well.
The [FM reference](fm_radio.md)
connects this callback to the FM client and its I²C probe. The historical Nokia
BCM2048 driver names register `29` as slave configuration, with enable bit
`80`; the FM probe expects address `22`. Together these strongly support
interpreting `a2` as enabling the FM I²C slave at `0x22`. The UART-to-I²C
register correspondence remains an inference until physical readback confirms
it. The `7f` case clears that enable bit, but its remaining bits and the
requirements for Bluetooth-only operation still need validation. The exact
packet templates are present in 10.12 and absent from the extracted 08.90
image; do not assume an identical gate sequence across both builds.

## Controller firmware and device identity

The 10.12 and 08.90 images contain different patches despite the common
`BCM2048B0 Initialization Configuration` label. Both use 37 `FC4C` Write_RAM
commands starting at controller address `0x85500`, followed by `FC4E` with
parameter `ffffffff`. Their downloaded sizes are 8,946 and 9,029 bytes.
These are controller patches, not host ARM926 code or interchangeable ROM images.

After launch, 10.12 sends the following configuration; 08.90 omits the first
row and sends the remaining eight with the same payloads:

| Opcode | Payload | Established meaning |
| --- | --- | --- |
| `FC0A` | `02 11 1f` | Vendor configuration, 10.12 only; detailed semantics unresolved |
| `0C43` | `00` | HCI Write Inquiry Scan Type |
| `0C47` | `00` | HCI Write Page Scan Type |
| `FC1C` | `00 00 00 00 00` | Vendor configuration; field meanings unresolved |
| `FC1E` | `00 00 00 00 00` | Vendor configuration; field meanings unresolved |
| `FC22` | `00` | Vendor configuration; field meanings unresolved |
| `FC61` | `01 19 1c 19 19` | Vendor configuration; field meanings unresolved |
| `FC0B` | `bf ee 0f ce 98 3d 59 82` | Vendor configuration; field meanings unresolved |
| `FC18` | `00 00 00 09 3d 00` | Two leading bytes, then little-endian 4000000 baud |

### Address handoff

10.12 `F04390C4` calls `F03CB1DA`, which requests the `BT_ID` record and copies
six bytes. If the record lookup does not produce the expected result, it
instead copies **`a5 a5 00 5b 02 00`** from `F1116B98` and still returns 1.
The archived emulator sends exactly that fallback in FC01; it is not evidence
of a physical handset identity. In usual displayed address order it is
`00:02:5B:00:A5:A5`. 08.90 has the corresponding getter at
`F0436BC8 -> F03C9802`.

The getter's success return therefore does not establish a valid identity.
FC01 takes six bytes in HCI wire order. Any Linux setup needs a valid per-device
address and must reject the known fallback; S40 pointers and private pairing
state cannot be retained as a Linux handoff.

## Linux prerequisites

The existing DMA provider handles linear, 32-bit LCD transmit only. It cannot
receive Bluetooth bytes or implement circular RX credits. RAPS serial, circular
DMA, GPIO/pinctrl and clock ownership are all missing.

Linux 6.12's `hci_nokia` provides H4+ framing but assumes a working serdev
controller and board setup. Its final baud is 3,692,300, whereas both inspected
S40 sequences select 4,000,000. Its firmware format is repeated
`le16 packet_size, packet_type, opcode16, plen8, parameters`; S40's reserved
byte, terminator and wire padding cannot be passed unchanged. Type 08 setup and
per-device address provisioning also need explicit handling.

The handoff stops circular DMA before Linux reclaims S40 RAM, but does not
establish a fresh Bluetooth session or prove the radio is off. Complete reset,
patch compatibility, actual TX-drain/RX-error semantics, clocks and RTS
backpressure remain unresolved. Bluetooth audio and [FM](fm_radio.md) have
separate routing requirements; HCI traffic alone would not establish either.
