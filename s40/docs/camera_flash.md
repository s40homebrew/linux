# Camera, imaging processor and flash

Camera and flash support are not implemented. RAPS controls the Julie imaging
processor (N1400 TMX320DM5012ZAN) over SPI and receives images over CCP. Julie
owns sensor I²C, sensor clock/shutdown and the ADP1653 flash. Neither the sensor
nor flash is directly reachable through a RAPS I²C device node.

The following wiring and protocols come from the service schematic and
08.90/10.12 firmware. RAPS register addresses are physical; embedded Julie
addresses belong to a different processor and are not Linux entry points.

## Physical topology

```text
RAPS GPIO/power/clock ----> Julie N1400 ----> sensor clock and shutdown
RAPS SPI control <-------> Julie          <--> sensor I²C at X1400
RAPS CCP receiver <------- Julie CCP TX    <--- sensor CCP data/clock
                            |
                            +-- local I²C --> ADP1653 N1450 --> flash LEDs
                            +-- GIO9 ------> flash enable
                            +-- GIO8 ------> flash strobe
                            <-- GIO10 ----- flash /FAULT
```

### Host and sensor connections

| Signal | Schematic connection | Consequence |
| --- | --- | --- |
| `CAM_VCTRL` | RAPS GPIO52; camera supply enables | Power management must coordinate the whole imaging domain |
| `RESET` | RAPS GPIO54 to Julie `RSTNX` B6 | Distinct from the sensor's shutdown line |
| `WAKEUP_INT0` | RAPS GPIO1 to Julie C4 | Host wake/handshake pulse |
| `JULIE_TO_HOST_INT` | Julie GIO2 C6 to RAPS GPIO0 | Completion/event notification is separate from SPI transfer completion |
| `CAM_CLK` | RAPS GPIO68 mux to Julie `SYSCLK` M1 | A working SPI controller alone does not start Julie |
| SPI clock, chip select | RAPS GPIO72/73 to Julie `SCLK0` A4 / `SDEN0` A5 | Board-specific pinmux and clock sequencing |
| SPI transmit, receive | RAPS GPIO63/62 to Julie `SDI0` C5 / `SDO0` B5 | Full duplex control transport |
| Host CCP | Julie `HSSIX_DP/DN/SP/SN` to `CCP_CMT` | Independent image-data receive engine on RAPS |
| Sensor CCP | X1400 differential data and clock to Julie `HSSIR_*0` | The sensor is not directly attached to RAPS's image receiver |
| Sensor I²C | X1400 pins 6/7 to Julie SDA0 M12 / SCL0 M11 | No proved transparent host I²C bridge yet |
| Sensor shutdown | Julie GIO4/INT2 P7 to X1400 pin 4 | Requires Julie ownership |
| Sensor external clock | Julie GIO17 P8 to X1400 pin 5 | Rate and startup depend on imaging firmware |

The camera rails are `VCAM_1V8`, `VCAM_1V3` and `VCAM_2V8`, supplied by
N1402 LM3677, N1403 LP5952 and N1404 BH28SA2. X1400 exposes VACT, VANA and
VDIG. The fitted sensor/actuator still requires handset identification.

### Flash connections

N1450 is `ADP1653ACPZ-R7`. Its SDA/SCL nets join the local camera I²C bus.
`EN`, `STR`, and `/FAULT` connect to Julie GIO9, GIO8, and GIO10 respectively.
The high-power LED circuit goes to X1450/X1451 on the flash board. The separate
`ILED` indicator output, pin 6, is marked **not connected**.

The flash uses seven-bit address **0x30**, represented as wire bytes `0x60`
and `0x61`. It is separate from the RAPS-side headphone amplifier at 0x60.

## RAPS power, reset and transport

### Open and close sequence

Host `10.12:F041C31A` / `08.90:F041B79E` serializes first-open through resource
84 and a reference count. Its first-open path performs the following operations:

1. Park the SPI pins/controller through `F0320EB0` / `F0321648`.
2. Configure GPIO52 as an output, drive it high, and call the delay helper with
   argument `500`.
3. Configure GPIO54 as an output, drive reset low, then delay with argument
   `40000`.
4. Configure GPIO1 as an output and drive it high. Configure GPIO0 for the
   incoming event path.
5. Select GPIO68's clock mux, write physical `0x20032c1c = 1`, and set clock
   control through `0x20032c84 = 0x20`.
6. Install the GPIO0 callback, reset protocol state, allocate an 8,212-byte TX
   buffer and 2,016-byte RX buffer, initialize SPI, then load image ID 0.

Pinmux uses set/clear aliases at
`0x20031c44/48`, `0x20031c14/18`, and `0x20031c54/58`. Copying only the output
levels omits the mux and clock dependencies.

Last-close calls `F041B8C0` / `F041AD44`: stop the event source, park SPI, drive
GPIO0 and GPIO54 low, clear the clock through `0x20032c88 = 0x20`, park GPIO68,
drive GPIO1 low, delay with argument `500`, drive GPIO52 low, and free the
transport buffers. This is a stock sequence, not yet proof that every image
has stopped writing or that a Linux buffer may be freed immediately.

`F03113A0` / `F0310B14` is a delay thunk. Preserve the numeric arguments as
evidence: the underlying calibrated busy loop depends on S40 clock state. A
Linux implementation must establish the actual required time and use Linux
delay/timing APIs, rather than call these addresses or copy loop counts.

### SPI register evidence

The main block is physical **`0x20020400`**, with another control/interrupt
block at **`0x20020600`**. `F0320DB0` / `F0321548` installs IRQ **160** and
writes secondary offsets `+04=1`, `+34=2`, `+28=2`, `+10=2`, `+20=2`.
The callback writes secondary `+20=2` again.

| Main offset | Stock observation |
| --- | --- |
| `+04` | Values 0, 1 and 2 select transport phases; transitions wait for `+28` |
| `+08`, `+0c` | Initialization writes 8 and 0 |
| `+10`, `+14`, `+24` | Initialization writes 1 |
| `+18` | Direction/phase-dependent value from the second timing argument |
| `+1c`, `+20` | Calculated clock low/high counts |
| `+28` | Busy condition, polled until zero |
| `+30`, `+3c` | Active transfer preparation writes 3 and 0 |
| `+34` | Initialization writes 2 |
| `+40`, `+70`, `+74` | Cleared during initialization |
| `+48` | Mode 3 writes 1; other recovered modes write 0 |
| `+4c`, `+58` | Timing reconfiguration writes 0 and 1 |
| `+5c` | Written zero, then polled until nonzero |

The timing helper `F0320DD0` / `F0321568` uses a source factor derived from
`0x20032c34`, a numerator 192 and mode-dependent divisors 10, 20, 12 or 27.
It divides the resulting count between `+1c` and `+20`. These arithmetic
constants do not alone prove a measured SPI frequency.

TX `F0320FDA` / `F0321772` submits `(buffer, endpoint 24, count, callback)` to
the S40 DMA helper. Full duplex `F032105A` / `F03217F2` first submits
`(endpoint 24, RX buffer, count, RX callback)`, then the TX side. Both sides
must be cancelled on setup failure or timeout. An existing RAPS DMA endpoint
number is not a Linux DMA channel number.

The S40 SPI wait helper starts a timer with argument `10000` and waits on an
OS resource; it also stops outstanding transfers. Its decompilation returns
1 after cleanup, so its return value alone is not a complete error contract.
Recover the controller status and completion semantics before replacing this
with Linux completions and bounded waits.

## Embedded Julie firmware

The embedded image IDs are 0, 4, 5, 6, 7, 8, 9, 11 and 12. Their complete
payloads match between the inspected firmware versions. ID 0 is the 7,297-byte
bootstrap; image 9 contains sensor/flash code. These proprietary images are
not included in the Linux distribution.

Every image parses exactly as a four-byte initial word followed by records:

```text
u32 little-endian length
if length == 0: end of container
u32 little-endian destination
u8 payload[length]
```

The initial word's execution semantics are unresolved; it must not be treated
as an ARM entry address for every image. Image 9 has identifiable ARM functions
in its load sections, including the sensor and flash routines below.

### Bootstrap versus later image loading

`F041BE6A` / `F041B2EE` selects an entry by ID. For ID 0 it raises reset,
walks the records after the initial word, masks each destination with `0x7fff`,
prepends that value as two **big-endian** bytes, and transfers the record payload
in SPI mode 3. The five bootstrap payload lengths are 65, 189, 6,761, 169 and
65 bytes, with destination fields `0`, `0x80`, `0x100`, `0xe34`, `0xe88`.
It toggles GPIO1 low/high after sending them.

For other IDs, the host first sends a command of type 3 with payload word zero,
then `F041C540` / `F041B9C4` streams the complete container in chunks of at most
`0x2014` bytes using SPI mode 2. Julie interprets the container records on this path.

The loader waits for incoming events and accepts status 4 as ready. Status 23
can cause another attempt, bounded at ten iterations in the stock loop.
Subsequent runtime command exchanges are required; transferring the bytes is
not sufficient initialization. Image-to-use-case selection is only partly
mapped: movie-related strings occur in images 4/5 and still-image/flash code in
9, but this does not define every mode, transition or compatibility condition.

## Runtime command and event framing

The host's ordinary SPI command buffer has a 20-byte header. Builders initialize
the following fields before `F041BC78` / `F041B0FC` assigns the outgoing sequence:

| Offset | Type | Recovered meaning |
| --- | --- | --- |
| `+0` | LE32 | Outgoing sequence number |
| `+4` | LE16 | Command type |
| `+6` | LE16 | Zero in requests; response/status code on received messages |
| `+8` | LE32 | Payload length |
| `+12` | LE32 | Zero in these request builders; full semantics unresolved |
| `+16` | LE32 | Constant `0x0000dead` in these requests |
| `+20` | bytes | Command payload |

Observed request/expected-reply combinations include type 1 / `0x8001`
(payload selector 18 or 19), type 3 / `0x8003` (image-change precursor), type
14 / `0x800e` (payload selector 28 or 29), and type 5 / `0x8005` (camera
configuration/control). Their complete state-dependent meanings still need
to be established. These are Julie SPI messages, not the DSP's F3 audio packets.

Most modes use a 532-byte transfer containing 20 bytes of header and up to 512
payload bytes. Some selected modes use 2,016 / 1,996 bytes instead. The host
allocates for both and keeps a count of outstanding requests; image ID 9 has
special handling for that count. Linux must preserve the distinction between
SPI transfer completion and an accepted/completed camera command.

`F041BA5E` / `F041AEE2` rejects duplicate/zero received sequence numbers as an
empty/stale response. It normally expects the next sequence and treats a delta
above five as an error, with a special first-transfer case. Type `0x8012` is
an asynchronous status. Types `0x010c` and `0x010d` request host participation;
the host prepares `0x810c` and `0x810d` replies. They are not camera-frame
notifications by definition, and ignoring them would leave the firmware waiting.

`F041C46A` / `F041B8EE` serializes inner camera fields as **LE16 tag, one-byte
length, bytes**. Values up to four bytes are copied from the scalar argument;
larger values use a pointer. Tag `0x0a00` is a terminator that finalizes the
outer payload length. The complete tag dictionary, including focus/flash
commands and mode-specific constraints, is not yet recovered.

## Capture and buffer ownership

The RAPS CCP receiver is physical **`0x20020700`**. The raw helper at
`F031B050` / `F031AC40` registers IRQ **115**; initialization starts at
`F031B068` / `F031AC58`. Recorded accesses include:

| Offset | Observed role |
| --- | --- |
| `+14` | Initialization writes `0x78` |
| `+18` | Bit 0 is cleared and set by the restart helper |
| `+1c` | FIFO/data port, read 64 times by the drain helper |
| `+20` | Status read; used by completion and error paths |
| `+24` | Interrupt/control mask operations using `0x3ff` and bit 3 |
| `+28` | Enable-like value 1 during init, 0 during stop |

`F031B7C2` / `F031B3B2` arms a receive transfer from S40 DMA endpoint **17**
into a camera buffer. The receive size derives from dimensions, format size
and additional camera data, expressed in words by the caller. The precise
hardware count programming and overflow handling require a receive-capable
DMA implementation. The existing LCD DMA provider supports memory-to-device
transfers and cannot supply this path unchanged.

The allocator `F031AF88` / `F031AB78` manages three buffers, aligns allocations
to 32 bytes, and rounds the requested word count to a multiple of eight. Some
stock modes instead use a reserved S40 region capped around `0x186a10` bytes.
Those S40 addresses and ownership flags do not survive Linux takeover.

The host dimension table contains 2048×1536, 1600×1200, 1280×960, 1152×864,
800×600, 640×480, 352×288, 320×240, 176×144, 160×120 and 128×96. This is a
capability/configuration table, not proof that all sizes stream successfully
on the fitted module. Other host routines perform RGB565 scaling and JPEG
byte handling; actual CCP framing, format ordering and compressed-payload
termination still need capture validation.

## Sensor detection and overlays inside image 9

The container carries alternate sensor implementations. Strings name primary
`KM4858002` / `MT9T013`, another primary group `ATSM2028` / `FOXCONN65` /
`SMIA65SH`, and secondary `TCM8330MD` / `VS6555`. These strings describe compiled
support/error cases, not an inventory of sensors fitted to the 6303.

An image-9 sensor probe prepares clock/shutdown registers and compares a
two-byte read of register zero with **0x2600**. The read descriptor encodes register
address in the upper bits and byte count in its low three bits, so this request
reads two bytes from register zero. It uses wire addresses `0x20` / `0x21`, a
two-byte register address sent high byte first, and high-byte-first readback.
On success it records sensor type `0x22`; a failed probe records zero.

The `0x2600` identifier agrees with the historical upstream
[MT9T013 driver](https://raw.githubusercontent.com/torvalds/linux/v2.6.35/drivers/staging/dream/camera/mt9t013.c),
which names register zero as the model ID. This supports identifying this
compiled probe as the MT9T013 branch. It does not identify the fitted module or establish this board's actuator
and clock configuration.

Image 9 installs alternate code/data overlays after sensor detection. A flat
disassembly of an overlay destination can therefore differ from the selected
runtime code. Sensor initialization, actuator commands, autofocus and focus
completion remain unresolved.

## Flash implementation inside image 9

The initialization writes are `OUT_SEL(0)=0`, `CONFIG(1)=0x1b`, and
`SW_STROBE(2)=0`, followed by `FAULT(3)` readback. The level function first
clears the GIO8 strobe bit, writes `CONFIG=0x19`, then chooses:

| Signed input level | Final CONFIG | OUT_SEL | Observed selection |
| --- | --- | --- | --- |
| `<= 0` | `0` | `0` | Outputs off; strobe remains low |
| `1..49` | `0` | `1` | Indicator code 1, no high-power LED current code |
| `50..99` | `0` | `8` | High-power LED code 1 |
| `>= 100` | `0x19` | `0x98` | High-power LED code 19, timer enabled |

For positive input it then sets GIO8. The input is a firmware level, not a
linear percentage or an arbitrary current request. Since ILED is unconnected,
the low positive branch does not imply a visible low-brightness flash mode.
Do not expose it as a continuous brightness scale.

The local Linux 6.12 ADP1653 definitions map high-power codes 1 and 19 to nominal
50 mA and 320 mA. Its timeout formula maps `CONFIG=0x1b` and `0x19` to roughly
219.4 ms and 328.6 ms. These interpretations agree with the chip register
layout, but board current, thermal limits, optical output and the exact pulse
duration still require measurement. The
[ADP1653 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/adp1653.pdf)
describes the timer, strobe and fault functions; the chip's maximum ratings are
not recovered RM-443 operating limits.

The flash routines access Julie-local GPIO control around `0x00030580`:
direction-like bits `0x200` and `0x100`, a set-style register at `0x3058c`, and
a clear-style register at `0x30592`. These match the schematic's GIO9 enable
and GIO8 strobe. Output-off still programs the enable-side bit and writes the
LED current selection to zero; it is not evidence of a full flash-chip or
camera-domain poweroff.

The read helper and initialization prove a fault-register path, but the complete
asynchronous GIO10 fault policy, recovery/retry limits, exposure synchronization
and host command that invokes each flash mode are not yet resolved.

## Implementation gaps

RAPS SPI, receive-capable CCP DMA and general GPIO/clock/power ownership are
missing. The existing LCD DMA provider cannot receive camera data. Complete
command tags, image transitions, asynchronous host requests and frame-boundary
semantics must be recovered before exposing V4L2 capture.

Linux must own fresh buffers and quiesce SPI/CCP transfers before freeing them;
S40 addresses and callbacks cannot survive takeover. Full-resolution two-byte
frames require 6 MiB each, so triple buffering would consume 18 MiB of the
30 MiB Linux bank.

Linux's ADP1653 driver is a V4L2 flash subdevice, but it cannot reach the chip
without a verified Julie bridge or translated camera command interface. Flash
current/timeout limits, GIO10 fault handling and exposure synchronization remain
unverified. Stock Julie firmware and a host flash driver must not control the
same registers concurrently.
