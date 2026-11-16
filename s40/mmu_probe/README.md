# ARM926 MMU capture

This separate MIDlet captures the ARM926 registers, the 16 KiB first-level
translation table, and second-level tables needed to translate six useful
addresses. It also tries a **6 MiB allocation**, falling back to 64 KiB if that
fails, and verifies physical continuity at every 1 KiB boundary. The allocation
is freed before returning to Java; its addresses must not be reused afterward.

Build from the Linux repository root:

```sh
bash s40/build_mmu_probe.sh
```

Artifacts: `s40/dist/Linux6303MmuProbe.jar` and matching `.jad`.
The Java shell rejects every model outside the shared RM-443 profile before
native execution, and uses the existing resolver/bootstrap. Choose **Check
firmware**, then **Capture MMU tables**.

## Capture sequence

The register-only `Linux6303Probe` does not change CP15 state. MMU capture
must temporarily enable access to the firmware's domain-0 table alias:

1. Read privileged CPU identity, control, TTBR, DACR and FCSE. Require an ARM926,
   MMU enabled, little-endian tables and a 16 KiB-aligned TTBR inside the board's
   32 MiB physical DDR envelope.
2. Find exactly one checked table-walker pattern in the shared profile's already
   validated first 64 KiB of ROM. Validate all six relative calls against the
   expected TTBR/control/DACR read, DACR write and write-buffer-drain helpers.
   No firmware function address is called or selected by a version string.
3. Save CPSR and mask IRQ/FIQ; read and check current CP15 state again. Save DACR,
   set domain 0 to client, and drain the write buffer as the firmware walker does.
4. Read L1 from VA `0xd0000000`. For targeted coarse/fine tables, derive the same
   alias as firmware: `table_pa + 0xd0000000 - TTBR`. Reject tables outside DDR
   or below the table pool's TTBR. No direct MMIO access or page-table
   write is performed. There are no firmware calls inside the critical interval.
5. Restore DACR, drain, restore CPSR control and return. Only then write optional
   files and prepare screen results through normal S40 services.

The probe has no data-abort recovery; a mapping outside the validated policy
can hang or reset the phone. S40 retains MMU, clock and device ownership.

## Results

The screen always shows complete values in fixed 32-character lines: capture
status, control/TTBR, CPSR/DACR, code/stack/allocation start/end VA-to-PA pairs,
and the allocation size, first physical address and range status. Range status
zero means the whole tested allocation translated continuously within DDR.

If the phone's native filesystem allows writing to `D:`, the probe creates:

- `D:\linux6303-mmu.json`: registers, checked walker location, translation
  descriptors/results, restoration values and allocation range result.
- `D:\linux6303-l1.bin`: 16 KiB L1 table, physical base given by JSON `ttbr`.
- `D:\linux6303-l2-0.bin` through `-5.bin`: only for targets using L2; each file
  has its physical base and length in the JSON target record.

Target indices are 0=code, 1=stack, 2=allocation start, 3=allocation last byte,
4=snapshot buffer, 5=shared launcher result descriptor. L2 files may duplicate
the same physical table. Deduplicate those before passing segments to
`tools/armv5_mmu.py`, which rejects overlapping physical dumps.
The JSON uses format 1.

If file writes fail, the screen still reports the key mappings. Failed or
partial output files should not be combined with data from an earlier run.

## Reusable native API

`../boot_probe/native/capture.c` is shared with the Linux launcher and can be compiled into the
loader. The caller must own all output buffers and target allocations.

```c
struct mmu_cpu cpu;
struct mmu_range range;
if (linux6303_mmu_cpu(&cpu) || !linux6303_find_table_walker())
    reject();
linux6303_translate_range(&cpu, allocation_va, allocation_bytes, &range);
if (range.status)
    reject();
/* range.pa corresponds to allocation_va; keep the allocation alive. */
```

`linux6303_translate_range` masks/restores IRQ/FIQ and DACR once for the whole
walk. Status meanings: 1=invalid input or changed CPU state, 2=L1 fault/reserved
firmware domain, 3=L2 table outside checked RAM pool, 4=invalid L2 descriptor,
5=translated address outside DDR, 6=physical discontinuity. `checked_bytes` and
`first_bad_va` identify the failure boundary. It checks the smallest ARMv5 page
boundary, including fine-table tiny pages and repeated large-page descriptors.
The result describes translation and contiguity for the lifetime of the
allocation. Device shutdown and Linux placement are handled by the boot loader.

## Component check

`python3 s40/mmu_probe/tests/check_capture.py` runs the compiled ARM capture code
in Unicorn with synthetic page tables. It checks translation, physical
continuity and restoration of DACR/interrupt state.
It requires LLVM, Unicorn and pyelftools.
