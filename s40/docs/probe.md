# Reusable RM-443 experiments

`/dev/rm443_probe` provides RAM buffers, configurable ROM calls, uploaded
ARM/Thumb routines, explicit-width memory reads and execution records.
`rmprobe` is its session client. Install and boot a kernel containing the
interface once; subsequent experiment programs and data can be uploaded over
USB without another kernel build. Registering the device does not run an
experiment at boot.

## Sessions and buffers

Run `rmprobe` at the phone's USB shell for an interactive `probe>` prompt, or
`rmprobe -f /tmp/experiment.probe` for a script. A script stops on its first
error. `rmprobe info` and `rmprobe -c 'read physical 0xffff0800 2 16'` are
single-command forms. `rmprobe --help` lists the syntax without opening the
device.

`info` still reports buffer limits if the retained ROM bridge is unavailable;
its bridge address fields are then zero. Physical reads and buffer transfers
remain usable, while ROM calls, execution and bridge-address reads report that
the bridge is unavailable.

Keep related commands in one session:

```text
alloc data 4096
upload data /tmp/input.bin
buffers
download data /tmp/output.bin 0 64
free data
quit
```

`alloc` creates zeroed, physically contiguous memory for ROM arguments. Its
per-object limit remains 1 MiB. A large contiguous allocation can fail on a
fragmented handset. There are at most 32 objects per session.

ABI 2 also provides two kinds of large object, up to 32 MiB each:

```text
paged scratch 2097152
flash image 0 20971520
map image
pages image
buffers
unmap image
```

`paged` allocates one zeroed RAM page at a time. `flash` creates a view of the
specified physical NOR range; the example covers the first 20 MiB of NOR,
not a discovered MCUSW image boundary. The flash base must be 4 KiB aligned.
Unmodified pages remain backed by NOR. Explicit uploads, imports and copies
materialize only their affected pages in RAM. These buffer operations never
write flash.
The page list identifies the actual physical backing and whether each page is
RAM or NOR. Resident accounting includes metadata and mapping tables as well
as data pages, so an unmodified flash view has a small nonzero RAM cost.

`map NAME` assigns an optional contiguous **bridge virtual address**. It does
not allocate a contiguous physical image. `unmap NAME` releases that mapping;
the underlying data and edits remain until `free` or session close. `buffers`
refreshes and displays object kind, resident bytes, physical address if one
exists, and bridge virtual address if mapped.

Address forms are explicit:

| Form | Meaning |
| --- | --- |
| `@name+OFFSET` | Physical address inside a contiguous `alloc` buffer |
| `%name+OFFSET` | Virtual address inside a mapped object, during probe execution or bridge reads |
| Numeric value | Literal address or integer; the caller supplies its correct address space |

`@name` rejects paged and flash-view objects, even when an unmodified view
currently has linear NOR backing. ROM calls reject `%name` references in
registers, descriptors and stack arguments. Public MMU overlays do **not**
change the ROM's physical-pointer contract and cannot automatically supply a
modified image for protected authentication. Stock images in NOR, small RAM
headers and small complete signed images remain suitable for those probes.

Mapped views are useful for public-context execution and inspection. Mappings
are present for raw probe execution even when the entry itself is a physical
`@code` address. `exec %code+1` selects a Thumb virtual entry; `execva ADDRESS`
provides the equivalent explicit form for a numeric bridge virtual address.
Untouched flash-backed pages must not be written by an uploaded routine:
there is no COW page-fault handler during execution. Materialize every page
that a routine will write using an upload or copy first.

The default session RAM budget is 4 MiB. Inspect or adjust it without rebuilding:

```text
budget
budget 8388608 4194304
```

The second value is the minimum available-memory headroom, at least 4 MiB. The
kernel checks current memory availability before allocations; the budget is
a limit, not a reservation or a promise that the next allocation will succeed.
It covers resident data, metadata and map tables. A 20 MiB logical flash view
does not reserve 20 MiB of RAM at boot. Increasing the budget does not remove
retained secure allocations or make a full MCUSW physically contiguous.

Uploads and downloads use chunks of at most 64 KiB. `upload` takes a regular
file; `download` creates a new file and refuses to overwrite an existing one.
The streaming commands below avoid making a complete second copy in `/tmp`.

Closing the session frees its buffers. **Do not close or free a buffer while
a ROM service retains a pointer to it.** The kernel preserves the existing
watchdog work buffers and its callback allocation bookkeeping separately.
Only one probe session can be open. Access requires `CAP_SYS_RAWIO` and the
device node is mode 0600.

Numbers are decimal or `0x`-prefixed hexadecimal. Quotes and backslash escapes
are supported in script filenames; there is no shell expansion or command
substitution. A `#` at the start of a word starts a comment.

## ROM calls and uploaded routines

ROM calls require an explicit 16-byte descriptor, supplied as four
little-endian words or an exact 16-byte binary file. The kernel copies it to
the bridge and puts its physical address in `r0`. All other registers default
to zero; stack arguments default to empty. The default entry is `0xffff0809`.

The following is a syntax template, **not a known-valid service request**:

```text
call descriptor=file:/tmp/service.desc r1=0x1f r2=@header stack=@request,@response,2
record
download response /tmp/response.bin
```

Use `descriptor=W0,W1,W2,W3` to specify the descriptor words directly, and
`entry=ADDRESS` for another mapped ROM entry. `r1` through `r12` can be supplied;
`r0` is reserved for the descriptor in `call`. At most 16 stack words may be
supplied, in increasing stack-address order. The kernel supplies no individual
authentication, FOTA, key-hash or watchdog experiment policy.

`exec` supplies all `r0`–`r12` directly and starts inside an owned buffer:

```text
alloc code 4096
upload code /tmp/return_args.bin
exec @code r0=7 r1=11
record
quit
```

The ARM example at `tools/probe_examples/return_args_arm.S` returns `r0=18`
and `r1=12` for these inputs. Its Thumb counterpart uses `exec @code+1`.
Entry bit 0 selects Thumb; an ARM entry must be word-aligned.

With LLVM tools on `PATH`, build either example from the Linux repository root:

```sh
mkdir -p s40/build
clang --target=arm-none-eabi -march=armv5te -c s40/tools/probe_examples/return_args_arm.S -o s40/build/return_args.o
llvm-objcopy -O binary -j .text s40/build/return_args.o s40/build/return_args.bin
```

Use the Thumb source filename instead for the Thumb example. Uploaded routines
must be position-independent or linked for their allocated address. They run
privileged in the public System context, with an 8 KiB private stack (less
argument staging and return bookkeeping) and the bridge's physical RAM, ROM
and captured firmware mappings. They must return through LR with SP, System
mode and the bridge mappings intact. Both IRQ and FIQ must remain masked.
They must not change exception-vector state, leave asynchronous hardware work
running, or corrupt the bridge or Linux memory. The bridge saves and restores
Linux registers, translation/control state and interrupt masks on normal return.
Uploaded code does not itself enter protected execution; that still requires
the ROM gateway.

Calls serialize with secure-watchdog service. Allocations and user copies
happen before the interrupt-disabled interval, and the bridge performs cache
maintenance. A routine or ROM call that hangs, faults, or changes the return
environment can prevent restoration and require a reboot. A normal Linux
timeout cannot interrupt that state; keep calls short enough for the hardware
watchdogs. The interface is a privileged research mechanism, not isolation
from erroneous experiment code.

## Reads and execution records

```text
read physical 0xffff0800 2 16
read bridge 0xffff0800 1 16 /tmp/rom-entry.bin
```

`physical` addresses physical memory. `bridge` resolves an address through
the bridge's private mapping table, then reads its physical backing while
Linux is running. Width must be exactly 1, 2 or 4 bytes; both address and
byte count must be width-aligned. Each operation reads at most 64 KiB, without
combining byte accesses into wider loads. Omitting the filename prints a hex
dump. Device-register reads can have hardware side effects.

Reads release the shared lock between pages, so a multi-page read is not an
atomic snapshot of state that the watchdog may update. For a consistent
snapshot, an uploaded short routine can copy the desired state into an owned
buffer during one execution, then download that buffer after it returns.

Recoverable load faults return the bytes completed, the failing address and
an error status. Output files contain only completed bytes. An MMIO bus stall
is not a recoverable CPU fault and cannot be bounded by this mechanism.

Every completed call prints its sequence number, final stage, elapsed
nanoseconds, return CPSR, `r0`–`r12`, and up to 32 callback records containing
arguments and results. Additional callbacks are counted as overflow. `record`
retrieves the latest execution record for this open session. A failed result
copy is not automatically retried, because execution may already have happened.
These records remain in RAM and are not a persistent crash recorder.

## Streaming and USB capture

`import NAME PATH OFFSET BYTES` and `export NAME PATH OFFSET BYTES` transfer
an exact length in bounded chunks. Paths may be regular files or FIFOs; export
refuses an existing regular file. `import NAME - OFFSET BYTES` consumes exactly
that many binary bytes from the command input, immediately after its command
newline. The next byte then belongs to the next command. Use this framing on a
pipe, not a terminal. A short import reports an error; earlier completed
chunks remain in the object.

`rmprobe --binary -f SCRIPT` routes **rmprobe's** text to stderr and permits
`export NAME - OFFSET BYTES` to write only bytes to stdout. This is suitable
for a separate pipe or network stream. The shared USB console also carries
kernel messages, so its raw stdout is not a binary-clean export channel.
Use `gethex NAME OFFSET BYTES` there; it produces bounded, offset-tagged
`data NAME OFFSET HEX` lines in the serial capture.

`copy NAME OFFSET NOR_PHYSICAL BYTES` copies directly from NOR into object
offsets in chunks, without an image-sized userspace file. It can initialize
a paged object or materialize flash-view pages before an uploaded routine
writes them. `puthex NAME OFFSET HEX` imports a bounded hex fragment directly
into an offset, including across page boundaries.

When `capture_phone_serial.py` owns the USB port, use its command queue. Start
a persistent session from the phone shell:

```sh
rmprobe --fifo /tmp/probe.commands &
```

Once it prints `ready FIFO /tmp/probe.commands`, open a shell writer FD:

```sh
exec 9>/tmp/probe.commands
```

Now ordinary queued shell commands can control the same open probe session:

```sh
printf '%s\n' 'flash image 0 20971520' >&9
printf '%s\n' 'puthex image 4095 01020304' >&9
printf '%s\n' 'gethex image 4095 4' >&9
printf '%s\n' 'buffers' >&9
```

`--fifo` creates a new mode-0600 FIFO and refuses to reuse an existing path.
It keeps the session open between separate writers. `quit`, or the first
command error, closes the session, frees its buffers and removes only its own
FIFO. An incomplete length-framed binary import into this self-held FIFO
waits for the missing bytes; use `puthex` for independently queued serial
commands. Terminating the process also releases its device buffers, though
an interrupted process can leave its FIFO pathname behind.

On the host, `tools/probe_stream.py` reads an input file incrementally and emits
`puthex` commands. `--shell-fd 9` wraps them in shell `printf` commands shorter
than 500 bytes, respecting the phone shell's line limit. It does not create an
object, execute code or open a serial port. To send through a running capture,
with the session, named object and shell FD already established:

```sh
python3 s40/tools/probe_stream.py image patch.bin --offset 4095 --shell-fd 9 --queue CAPTURE_DIRECTORY/commands
```

This queues one chunk at a time, respects the logger's shell-prompt pacing,
and waits for the exact `rmprobe` upload acknowledgement before advancing.
It stops on a disconnect, stream error or timeout and never retries a consumed
command. `.sent` files remain as capture evidence. Without `--queue`, it only
prints commands, which can instead feed a dedicated command stream. This
avoids a full input image or giant shell heredoc on the phone; large transfers
still take time over serial, and sparse patches or direct NOR copies are
usually smaller.

Finish the session and close the shell writer explicitly:

```sh
printf '%s\n' 'quit' >&9
exec 9>&-
```
