# Component checks

Run the checks for the code being changed, from the Linux repository root:

```sh
python3 s40/tests/test_mmc_write.py
```

The checks execute C helpers, ARM instructions and host utilities with synthetic
inputs. Firmware images and a connected handset are not required.

| Area | Checks and purpose |
| --- | --- |
| Boot | `test_boot_fdt.py`: native DT buffer bounds, reservations and `dtc` round trip; `test_relocation.py`: stackless ARM copy, overlap and BSS |
| Display | `test_lcd_pixels.py`: pixel packing and guarded tails; `test_lcd_transfer.py`: PIO output and stalled FIFO; `test_boot_marker.py`: ARM rendering and bounded waits |
| Storage | `test_mmc_reset.py`, `test_mmc_poll.py`, `test_mmc_write.py`: reset, clock fallback, FIFO progress and failures; `test_sdlog.py`: logger file/socket lifecycle |
| USB | `test_usb_mmio.py`, `test_usb_attach.py`, `test_usb_reset.py`, `test_usb_boot_wait.py`: register access, reset, attach and console readiness; `test_usb_console_submit.py`, `test_usb_init_order.py`: upstream patch behavior |
| CPU takeover | `test_takeover_quiesce.py`: ARM shutdown routine with stuck registers; `test_irq_drain.py`: interrupt storm limits |
| Probe interface | `test_probe_driver.py`, `test_probe_mappings.py`: ownership, allocation rollback and page tables; `test_probe_bridge.py`, `test_probe_assembly.py`, `test_callback_veneer.py`: staging and ARM ABI; `test_probe_cli.py`, `test_probe_stream.py`: client and transfer behavior |
| Diagnostics | `test_inspect.py`: bounded inspector reads; `test_host_tools.py`: MMU table and RAM-log decoding; `test_secure_schedule.py`: watchdog request/renewal policy using a simulated service |

Most checks need a host C compiler. ARM checks also need LLVM Clang, LLD,
`llvm-objcopy`, `llvm-objdump`, `llvm-readelf`, `llvm-nm`, and Python `unicorn`. The DT round trip
needs `dtc`. Set `LLVM` to an LLVM tools directory if they are not on `PATH`.

Two standalone checks live with their tools:

```sh
python3 s40/kernel/test_dts.py
python3 s40/mmu_probe/tests/check_capture.py
```

The first checks the host DT generator's layout validation. The second executes
the MMU capture code against synthetic tables; it additionally requires
Python `pyelftools`.

The full component suite is available when a change spans several components:

```sh
python3 -m unittest discover -s s40/tests -p 'test_*.py'
```
