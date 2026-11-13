# Upstream sources

The build uses these pinned revisions:

| Source | Revision | License |
| --- | --- | --- |
| [Linux 6.12](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git) | `adc218676eef25575469234709c2d87185ca223a` | GPL-2.0; per-file notices and exceptions |
| [BusyBox](https://git.busybox.net/busybox) | `fc71374dfccd46448c62947269a35f1420d7ee28` | GPL-2.0-only |
| [musl](https://git.musl-libc.org/git/musl) | `7813e9119eb5fe681b6750b8a033a0490bca0941` | MIT and retained notices |

PID 1 and several utilities use Linux's nolibc headers under their MIT option.
See `kernel/userspace/NOTICE` and `boot_probe/licenses/NOLIBC.txt`. The shared
S40 launcher/runtime is MIT; its newlib/GCC notices are included by the SDK
packager.

The [Nokia E6 Linux project](https://github.com/fluffleswaffles/rapuyama-linux)
was a reference for runtime takeover.
