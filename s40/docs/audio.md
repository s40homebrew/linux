# Audio on the Nokia 6303 classic

Audio is not implemented in this Linux port. S40 uses the RAPS DSP for PCM and
tones, AVilma for analog routes, and a TPA6130A2 for wired headphones. The
contracts below come from 08.90/10.12 firmware and board wiring. DSP timing and
processing inferred from an emulator remain unverified on hardware.

Register addresses are physical unless labelled as firmware addresses.
Protocol values are hexadecimal; GPIO/IRQ numbers and byte/sample counts are
decimal. Payload offset zero is the transaction byte after the eight-byte
Phonet header/object prefix. Multi-byte payload fields are generally big-endian;
PCM samples and firmware words are little-endian.

## Physical outputs and signal paths

| Endpoint | Schematic wiring | Consequence for Linux |
| --- | --- | --- |
| Loudspeaker / hands-free / music speaker | AVILMA B2 `HFSpP`, C3 `HFSpN`; filtered `HFSPP/HFSPN` reach J2165/J2166 contacts | Requires the AVILMA loudspeaker route, gain and power sequencing; not a TPA6130 output |
| Call earpiece | AVILMA C2 `EarP`, D3 `EarN` reach B2103 through R2504/R2505 and L2500/L2501 | Separate mono receiver route; selecting it does not implement telephony |
| Wired headphones | AVILMA `XEarL/XEarLC/XEarR/XEarRC`, coupling capacitors C2022/C2020, N2000 differential inputs; `HPLEFT/HPRIGHT` go through Z2002 to X2101 | Analog source plus external stereo amplifier; no PCM input on the TPA6130 |
| Microphone | B2150 ADMP421 digital microphone, `DMIC` wiring, AVILMA `MicData`/`AudClk` and analog microphone/accessory pins | Capture needs its own source, clock, routing and DSP-buffer work |
| Vibration | AVILMA `VibraP/VibraN` drive M2101 | Separate actuator, not a speaker or audio PWM substitute |
| FM / Bluetooth audio | BCM2048 FM analog lines and PCM/I²S signals on sheets 3/6 | Additional source routing and clocks, independent of Bluetooth HCI enumeration |

The six-signal `DIG_AUDIO` connection includes RAPS `AudioClk`, `EarDataL`,
`EarDataR`, `MicData`, `PMARP` and `PMARN`, connected to corresponding AVILMA
pins. RAPS `I2SSCLK`, `I2SWS`, `I2SSD1/I2SSD2` and the separately labelled
`PCM` nets also exist. Their names do **not** establish an ARM-accessible
standard I²S controller, sample framing, clock ownership or DMA request number.
In particular, do not substitute OMAP McBSP registers for an unrecovered RAPS
audio interface.

The high-level stock path is:

```text
S40 player / game synthesizer / PCM mixer on ARM
    -> AudioSrv configuration and shared PCM blocks
    -> MDI mailbox / resource F3 on the DSP
    -> RAPS audio interface and AVILMA routing
    -> loudspeaker, earpiece, or external headphone amplifier

S40 key/alert tone control
    -> F3/00 frequency and enable updates
    -> DSP tone generator
    -> selected analog output path
```

The mailbox and ARM-side buffer operations are recovered much more completely
than the DSP-to-AVILMA portion. A full AVILMA audio register map, digital-port
configuration, sample clock setup and physical mute/unmute sequence remain
open work.

## Headphone amplifier and I²C

### GPIO and controller

10.12 `F011BB08`, and 08.90 `F011B3DC`, configure **GPIO45 as output**, drive
it low, wait through the delay helper with argument 2, then drive it high.
The existing delay analysis identifies microseconds. The schematic connects
this control to N2000's active-low `_SD` pin D4; high enables the device.
Its supply comes from **VBAT** through L2023, rather than a discovered
software-switchable headphone regulator.

The I²C controller is at **PA `20032200`**, IRQ **123**, with an older OMAP-like
register layout. S40 maps it at `FFD32200`.

| Offset | Recovered operation |
| --- | --- |
| `00` | Revision; initializer accepts `21`, `22`, `31`, `32`, `36`, `38` |
| `04` | Interrupt enable; stock initialization writes `003f` |
| `08` | Status, W1C; bit 1 NACK, bit 2 access-ready, bit 3 RX-ready, bit 4 TX-ready |
| `10` | Reset-done status, bit 0 |
| `18` | Transfer byte count |
| `1c` | Data register; stock accesses use 32-bit MMIO with 16-bit FIFO semantics |
| `20` | System configuration; bit 1 software reset |
| `24` | Control; bit 15 enable, bit 0 START, bit 1 STOP |
| `28`, `2c` | Own address and slave address |
| `30`, `34`, `38` | Prescaler and SCL low/high counts |

Initialization uses prescaler 3 and SCL low/high 55. The archived model uses a
48 MHz functional clock, yielding 100 kHz with the controller's extra clocks;
the 14/6 setting corresponds to 400 kHz in that model. Measure/establish the
actual RAPS clock source before turning those into a Linux clock declaration.

`F04552DC` also changes bits 13:12 of `20032c10` to `10b`, acquires clock
resource 4 and calls a shared clock helper. `F04552B4` releases those references
under firmware policy. The initializer writes `10000000` to `20031c24`.
Preserve those dependencies when adapting the bus; copying the timing counts
without the pin/clock work is insufficient.

### Probe, enable, volume and shutdown

The seven-bit slave address is **0x60**, not an eight-bit read/write address.
S40 first attempts a raw five-byte read, then `F011BACE` checks for two
amplifier variants. For the TPA branch it explicitly reads register 4 and
requires bits 5:4 to be zero. The firmware's internal ID is `0x10000`;
`0x10001` selects an alternate implementation, not a second fitted chip proven
by the board schematic.

`F011B970` handles raw transfers and `F011B882` register reads/writes, using
OS resource 74 and completion callbacks. They retry rejected submissions up to
25 attempts. Those waits and callbacks belong to S40 and cannot be called
after Linux takes over. Linux needs normal I²C transfer errors and bounded
timeouts instead of firmware resource waits/assertions.

The observed TPA branch has the following sequences:

| Function, 10.12 | Register activity | Interpretation |
| --- | --- | --- |
| `F011BA9A` | `R1=c0`, `R2=c0`, `R3=03`, then `R1=01` | Initial control programming followed by software shutdown |
| `F011BB68` | GPIO45 low/high; `R1=c0`; clear R3 bits 1:0; clear R2 bits 7:6 | Re-enable both channels and remove output mute/high-impedance settings |
| `F011BBBC` | Step cached volume toward target through R2, with timer/resource waits | Gain transition; volume is a table index, not a linear percentage |
| `F011BCC8` | Ramp to index0; set R2 bits 7:6; `R1=00`; GPIO45 low | Mute/disable, then hardware shutdown |

The control meanings agree with [TI's TPA6130A2 datasheet,
SLOS488F](https://www.ti.com/lit/ds/symlink/tpa6130a2.pdf): registers 1/2/3
provide enable/shutdown, gain/mute and output impedance controls. Hardware
shutdown resets control state; software shutdown preserves selected settings.

The 64-entry float gain table at `F0E1E980` spans **-59.5 to +4 dB**. Its
08.90 counterpart at `F0E13F80` is byte-identical; the alternate table also matches between the inspected builds. Zero volume index is therefore the lowest gain, not
a substitute for the mute bits or shutdown.

`F02A3394` receives signed levels in half-dB units. On ordinary DSP routes it
returns the level for the DSP. On the external-amplifier branches it divides
by two, uses `F011BD44` to choose an amplifier index, and returns zero as the
residual DSP level. Applying the original attenuation again in software would
double-attenuate the signal.

There is a real version difference: **10.12** `F011BD44(float, selector)`
subtracts the value returned by `F0127F44` for selector 1 before its table
search. That getter reads `D803CA38`; the source and calibration meaning of
that float have not been established here. **08.90** `F011B618(float)` has no
selector or subtraction. Its caller `F02A5340` is correspondingly different.
Do not replace this with a guessed universal gain offset.

## DSP transport and boot ownership

The DSP protocol is a prerequisite for the stock playback path. It is not a
function-call interface into S40 and is not Linux rpmsg/virtio.

### Registers and shared queues

| Physical address | Contract recovered from MCU code |
| --- | --- |
| `20040280` | IPC status |
| `20040284` | Set IPC bits |
| `20040288` | Clear IPC bits |
| `20040304` | DSP memory-control word, initialized to 1 |
| `20032d18` | DSP control: stock release writes 1; runtime reset semantics only partly established |
| `08003800` | Host TX mailbox base |
| `08003c00` | DSP RX mailbox base |
| `08003f00` | 256-byte DSP bootstrap copy |

The MCU helpers `F042582E`, `F042585E`, `F042588E`, `F04258F2` establish
16 physical slots of 16 bytes each. A slot is at
`queue_base + 4 + 16 * (sequence & 15)`, with sequence numbers modulo 32.
The host word at TX base holds TX producer in low 16 and RX consumer in high 16.
The DSP word at RX base holds RX producer in low 16 and TX consumer in high 16.
Use the sequence state to distinguish full from empty; matching slot indices
alone are insufficient.

| IPC bit | Meaning | IRQ |
| --- | --- | ---: |
| `01` | Host has published TX work | Doorbell to DSP |
| `02` | DSP has published RX work | 19 |
| `04` | Host consumed RX descriptors | Acknowledgement to DSP |
| `08` | DSP consumed TX descriptors | 21 |
| `10` | Host acknowledges boot/restart phase | Acknowledgement to DSP |
| `20` | DSP boot phase or runtime restart event | 23 |

Descriptors use flags at byte 11 and control command at byte 10. Compact Phonet
packets use flags 04 and pack their small payload around those control bytes.
Long TX uses flags 06 and a pointer/offset in word 3. Long RX uses flags 16,
with the allocation handle in word 0 and a matching big-endian handle prefix
before the packet in shared RAM. `F042540C` validates that handle before
posting the packet to the worker.

**Consuming an RX descriptor does not free its long-packet storage.**
`F04250D6` queues allocations for release; `F0424FC4` returns three handles
at a time in words 0/1/3 of a flags 19, command 02 descriptor, whose word 2 is
`19020007`. Descriptor credits and payload ownership are separate; advancing the RX
consumer does not authorize reuse of long-packet storage.

### Address units and reservation

`F0424E92` obtains boot records 5 and 16, translates the shared mapping and
derives DSP offsets relative to **PA `10000000`**. Archived 10.12 state has
shared offsets `[00140000,001c0000)`, corresponding to physical
`[10140000,101c0000)`. Its ARM alias base was `E06C0000`, so an offset was
accessed at `E06C0000 + offset`. That alias is a captured mapping, not a Linux
address formula. `F0425214` bounds a copy in the negotiated shared window and
then performs a plain byte copy.

PCM ring addresses in F3/31 are **byte offsets** in that DSP address space.
The F3/65 parameter-buffer response instead contains a **word address**,
which `F011E888` multiplies by 2. Mixing the two units would corrupt memory.
Neither is an arbitrary Linux physical or virtual pointer.

The current Linux memory bank starts at `10200000`, leaving the low 2 MiB out
of its page allocator. That protects the observed shared window, but does not
prove the DSP has no other DDR mappings, firmware pointers or bus-master
activity. Explicitly inventory and reserve every region required before
restarting it.

### Startup and reset

`F042551C` requires boot record 5 to cover at least `0x4000` bytes and record 16
at least `0x40000`. It clears mailbox storage, writes the memory-control word,
and copies 256 bytes from `DSP0 + 0x400` to the bootstrap area, rotating each
32-bit word by 16 bits. Startup also modifies bootstrap flags. Copying the
whole DSP0 image as unmodified ARM words would not reproduce that sequence.

`F0424E92` looks up `DSP0`, initializes mappings/resources, installs the boot
IRQ handler and releases the DSP through `F0425686`/`F042567E`. The first IRQ 23
handler `F0424E66` acknowledges IPC10 and installs the next phase. The second,
`F0424DE2`, initializes queues and submits four configuration words:

```text
shared reservation size, 01020304, 18010000, DSP-visible shared base
```

Runtime then uses separate RX/TX/restart handlers. An additional IRQ 23 is not
an innocent generic "ready" notification: the runtime path can enter restart
handling and disable receive processing.

## Audio service protocol

S40 audio packets typically start:

```text
12 7c 00 f3  LL LL  06 48  TT MM ...
```

This identifies media 12, DSP destination 7c, source 00, resource f3, a big-endian
length, destination/source object bytes 06/48, then transaction and message.
The length counts bytes after the six-byte header, including the object bytes;
total packet length is `6 + length`. Accessory requests use source object 34
instead of 48. Interpret headers before applying payload offsets.

| Request → response | Function | Evidence / restrictions |
| --- | --- | --- |
| `00` → unproved | Tone configuration | 22-byte payload; one-way handling in current emulator |
| `1b` → `1c` | Physical/audio configuration subblocks | Conditional S40 route-completion flag; analog meaning only partly decoded |
| `1d` → `1e` | Accessory parameter subblocks | Separate accessory state machine |
| `30` → `31` | Discover four PCM rings | Offsets and lengths must describe actual reserved storage |
| `32` → `33` | Configure/disable a ring | Echo transaction/ring; status is checked by the MCU |
| `34` → `35` | Announce playback data / report consumption | Consumer byte offset is authoritative for completed blocks |
| `36` → `37` | Capture-space notification / captured data | Separate producer direction; physical capture unverified |
| `3b` → `3c` | Configure a logical route | Route/source/sample/format/endpoint records |
| `3d` → `3e` | Remove a route | Pending playback completion must still be resolved |
| `3f` → `40` | Connect/disconnect the stream graph | Destination/count/source records terminated by `00ff` |
| `51` → `52` | Processing configuration | Counted subblocks, including fill policy and processing coefficients |
| `64` → `65` | Discover shared parameter buffer | Word address, capacity and status; separate from PCM-ring discovery |

### Physical routing and volume

`F02A355A` builds F3/1b as transaction, message, zero, subblock count, followed
by blocks with a big-endian 16-bit ID and total length. The archived initial
request includes IDs `00,01,02,03,04,05,06,07,08,23,09,3b,7d`. These are
configuration IDs, not AVILMA register addresses.

Subblock 02 has length 20 and signed big-endian left/right levels at offsets
10/12 within the block. The amplifier/DSP split described above establishes
their half-dB interpretation. Other fields select routes and activity policy,
but numerical values have not all been mapped to physical earpiece, speaker,
headphone and microphone states. Do not assign ALSA names to guessed bits.

Response handler `F012B18C` reads no status field. Only when `D803EF4E == 1`
does it send a local completion and clear that flag. A Linux client must define
and validate its own pending transactions rather than reproduce that global.

Parameter-buffer request `F011E83A` emits `TT 64 05 06 02 bf`, requesting
703 words. In the response, payload+4 is a big-endian 16-bit status; +6 is a
big-endian 32-bit word address; +10 is capacity in words. `F011E888` accepts
status 0 and sufficient capacity, doubles the address and stores it. The
associated transfer size is 1,406 bytes.

### PCM configuration and representation

`F0130F68` consumes a 36-byte F3/31 payload: ring offsets at +4,+8,+12,+16
and byte lengths at +20,+24,+28,+32, all big-endian 32. It does not validate an
error status or establish safe bounds for Linux. Validate nonzero sizes,
alignment, overflow, non-overlap and containment in the reserved DSP window.

`F0133D48` maps S40 stream types to the following geometry:

| Ring | S40 type, decimal | Bytes per DSP block | Main evidence |
| --- | ---: | ---: | --- |
| 0 | 17 | 1,920 | Playback; two 960-byte planes, signed 24-bit values in LE32 containers |
| 1 | 18 | 960 | Capture direction;480-byte per-channel field; complete capture format/rate still needs validation |
| 2 | 33 | 320 | Playback path; `F0134308` converts common samples to 16-bit by arithmetic shift 8 |
| 3 | 34 | 320 | Capture direction; endpoint/source semantics still need validation |

Do not extrapolate ring 0's rate, channel count or ALSA format to the other
rings. Their block sizes alone do not prove those properties.

Ring 0 is the existing emulator's **48 kHz stereo** path: each plane holds
240 samples and a block represents5 ms at that rate. `F013477C` copies the
two planes separately; `F04248A2` reaches the plain shared-memory copy.
`F0134308`'s unity-gain conversion to S16 shifts the common signed 32-bit
sample right 8. Together these support signed 24-bit/right-aligned normalization,
not interleaved S16 or full-scale S32. The archived model's rate/timing and WAV
checks support that working interpretation; the physical sample clock still
needs measurement.

The captured F3/32 ring 0 request payload is:

```text
01 32  0000  0002  0001  0003  0200  0008
TT MM  ring  mode  fmt   chan  word  block-count
```

`F01311EC` serializes those fields through ROM lookup tables. The map at
`F0E21282` contains ring IDs0..3, modes0..2, formats0/1, channel codes 0/3,
and sample-word codes 2/0100/0200. Internal channel selector 1 maps to wire 3;
it does not mean three audio channels. These 28 bytes are identical at
08.90 `F0E16882`.

The example's eight-block ring 0 capacity is 15,360 bytes, or 40 ms at 48 kHz.
Ring capacities and addresses in emulator traces are model choices; actual
values must come from validated DSP discovery.

### Logical routes, processing and startup ordering

The captured startup configures logical route 8/source 3/endpoints 12,13 and
route 9/source 2/endpoints 14,15. It then configures route 4/source 0/endpoints 6,7
for the type 17 playback path. F3/3b contains transaction/message, BE16 route,
source, sample-word code, format code, endpoint count, endpoint IDs and padding.
These endpoint numbers describe a DSP graph; route 4 is not by itself proof
that the loudspeaker has been selected.

The F3/3f example is a 130-byte payload. After transaction/message, its
big-endian 16-bit graph records encode:

```text
destination 12 <- source 6
destination 13 <- source 7
destination 0  <- source 14
destination 1  <- source 15
00ff terminator, then zero padding
```

Each record contains destination, source count and that many source IDs.
It is not a flat route list. An empty graph contains the terminator and
padding and is used on shutdown. The ROM table at `F0E7EE40` retains the
underlying little-endian graph data; the wire packet is serialized, not copied
verbatim from that table.

Before connecting the graph, the captured firmware sends F3/51 processing
blocks. Its header is transaction/message, BE32 processing mask, BE16 block
count. The observed mask is 3. Subblocks have BE16 ID and length:

| ID | Length | Recovered content |
| --- | ---: | --- |
| `72` | 8 | BE16 fill policy 0/1 at +4; reserved zero word at +6 |
| `74` | 24 | Global five-band processing values; Q14 unity 4000, smoothing 13a6/0044, Q15 unity 00008000 in the captured defaults |
| `75` | 84 each | Five numbered coefficient/band records |
| `33` | 68 | Companion processing parameters |

`F0132B80`, `F0132C7C`, `F0132E04` serialize the processing records. Their
examined bodies are byte-identical to 08.90 `F0132378`, `F0132474`, `F01325FC`.
That does not make the filters identity filters. The actual calibration/processing requirements and a usable bypass remain
unresolved.

### Progress, backpressure and stopping

`F013158C` builds a 14-byte F3/34 payload:

| Payload offset | Field |
| --- | --- |
| 0,1 | Transaction, message 34 |
| 2 | BE16 ring |
| 4 | BE32 producer byte offset within ring |
| 8 | BE16 announced DSP block count |
| 10,12 | BE16 format and sample-word codes |

The count is the minimum of unsubmitted queued blocks and **half the ring's
capacity**. Producer position can therefore be ahead of the announced work.
Consume only the announced blocks from the previous consumer, not every byte
up to the current producer.

F3/35 returns transaction, message 35, BE16 ring and BE32 new consumer offset.
`F0130E10` accepts playback rings 0/2, subtracts the actual circular advance from
queued bytes, updates submitted-byte accounting and clears pending masks 0x10/0x800.
An unchanged consumer means zero progress. At the working ring 0 rate,
one 1,920-byte advance accounts for 240 ALSA frames; a half-ring batch of four
blocks accounts for 960 frames or 20 ms. Host enqueue time is not DAC time.

Closing a ring with F3/32 mode 0 clears a different pending bit from F3/35.
Outstanding playback consumption must be resolved before acknowledging ring
shutdown; resetting ring indices alone leaves accepted work unaccounted for.

A DSP reset invalidates in-flight handles and transactions. Storage remains
DSP-owned until completion or verified quiescence, including during STOP.

Capture uses F3/36 and F3/37 (`F01317E8`, `F0130D68`). In that response handler,
equal old/new producer indices are interpreted as a **full ring**, unlike
playback's zero-progress case.

There is also a shared counter directory at boot-record 5 SRAM, independent
of PCM rings. `F04252B6` searches entries and `F0425314` checks the version
and repeatedly reads a cached value until two reads agree. The type 17 position
path ultimately selects counter ID 2. Counter units, epoch and update cadence
are not established here. Do not advertise a precise hardware timestamp or
use it as ALSA's playback cursor until that contract is recovered.

## Tone command

10.12 `F02A3DB8` builds a 22-byte payload and `F02A697A` sends it to resource f3.
08.90 counterparts are `F02A5CBC` and `F02A88DC`. Layout:

| Offset | Bytes | Meaning |
| --- | ---: | --- |
| 0 | 1 | Transaction, observed 1 |
| 1 | 1 | Message 00 |
| 2 | 2 | BE16 flags, observed 0000/00e0/00e1/00e3; bit 0 is enable |
| 4 | 2 | BE16 table-selected amplitude/gain parameter; exact law unresolved |
| 6,8,10,12 | 2 each | Four BE16 frequency values, units 1/4 Hz |
| 14,16,18,20 | 2 each | Four BE16 shape words; DSP interpretation unresolved |

`F02A3D84` multiplies the selected frequency by 4 and clamps it to 16,000;
zero disables a component. The corresponding maximum is 4,000 Hz. The separate
table-frequency helper `F02A69F2` has the same scaling/clamp. A captured
`0f70` frequency means 988 Hz; `0e10` means 900 Hz. The ROM shape tuples are
`{0000,ff0a,ff00,000a}` and `{0000,ff0a,3f50,000a}` and are byte-identical
across the two builds.

There is **no recovered duration field**. S40 sends subsequent update/disable
packets to end a note. Tone amplitude law and waveform/shape interpretation are not recovered.
Emulator sine-wave output does not establish the handset waveform.

## Implementation gaps

The current handoff clears DSP release before reclaiming S40 memory. DSP boot,
complete shared-memory reservations, calibration loading and a physical idle
acknowledgement remain unresolved; old S40 callbacks and buffers cannot be
retained as a Linux audio interface. The 30 MiB Linux bank excludes the known
low-memory shared window, but that does not establish all DSP dependencies.

Physical AVilma route values, audio clocks and mute/power sequencing are also
incomplete. A direct ARM audio implementation would require recovery of its
MMIO, DMA and framing; the board's I²S/PCM net names do not establish those.
The TPA6130A2 driver in Linux controls only an analog amplifier, not PCM input.
RAPS I²C/GPIO integration is a prerequisite for it.

Once these contracts are known, ALSA can expose playback with position derived
from consumed DSP blocks rather than host enqueue time. Capture, accessory
detection, FM, Bluetooth voice audio and telephony require separate work.
The existing RM-839 S40 homebrew audio adapter is a different platform and is
not an RM-443 Linux driver.
