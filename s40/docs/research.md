# Optional research tools

Kernel and JAR builds, and the [component checks](../tests/README.md), do not
require firmware images, phone captures or an emulator. Keep those inputs outside
this repository. The maintained source, hardware findings and build tools are
public; local research inputs are not distributed.

Host tools use these optional path settings:

| Setting | Default |
| --- | --- |
| `S40_SDK_ROOT` | `main/` beside this Linux checkout |
| `S40_RESEARCH_ROOT` | The directory containing the Linux checkout |
| `S40_LINUX_RESEARCH_ROOT` | `$S40_RESEARCH_ROOT/analysis/linux_6303/research` |
| `S40_LINUX_STATE_ROOT` | `$S40_RESEARCH_ROOT/analysis/linux_6303/emulator` |
| `S40_EMULATOR_ROOT` | `nokia_6303c_emu/` beside the research workspace |
| `QEMU` | `$S40_EMULATOR_ROOT/vendor/qemu/build/qemu-system-arm` |

Export overrides for a different layout. The emulator is a separate project;
its firmware and phone storage must already be configured. The session tool
uses that existing storage, so stop the graphical frontend before starting it:

```sh
python3 s40/tools/emulator_session.py start
python3 s40/tools/emulator_session.py screen
python3 s40/tools/emulator_session.py key ret
python3 s40/tools/emulator_session.py stop
```

Install JAR/JAD files with the emulator's `scripts/phone_files.py` while stopped,
or through its live card-detach file service. Capture the boot trace and RAM
log with `python3 s40/tools/capture_boot_log.py NEW_DIRECTORY`.

For a physical phone, `python3 s40/tools/capture_phone_serial.py NEW_DIRECTORY`
reconnects to the Linux USB serial device and records raw data and events.
Place shell commands in numbered `commands/*.txt` files under that directory;
the logger sends them at the prompt and renames each to `.sent` before sending.
Consumed commands are not replayed after disconnect. See [probe streaming](probe.md#streaming-and-usb-capture)
for larger transfers.

Emulator results describe the implemented hardware model. Use handset
observations to assess physical behavior and stability.
