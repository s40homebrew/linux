#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
busybox_commit=fc71374dfccd46448c62947269a35f1420d7ee28
musl_commit=7813e9119eb5fe681b6750b8a033a0490bca0941
runtime=/work/runtime-$(uname -m)
mkdir -p "$runtime" /work/initramfs
# Export immutable source objects to the case-sensitive Linux build volume.
for project in busybox musl; do
    if [[ $project == busybox ]]; then revision=$busybox_commit; else revision=$musl_commit; fi
    path="$runtime/$project-$revision"
    if [[ ! -f "$path/.source-commit" ]]; then
        [[ ! -e "$path" ]] || { echo "Unmarked runtime source: $path" >&2; exit 1; }
        mkdir "$path"
        git -c safe.directory="/$project-reference" -C "/$project-reference" archive "$revision" | tar -xf - -C "$path"
        printf '%s\n' "$revision" > "$path/.source-commit"
    fi
done
musl_source="$runtime/musl-$musl_commit"
busybox_source="$runtime/busybox-$busybox_commit"
busybox_objects="$runtime/busybox-objects"
musl_objects="$runtime/musl-objects"
musl_prefix="$runtime/musl-install"
mkdir -p "$musl_objects" "$busybox_objects"
# Preserve installed libc, objects and archive across all kernel-only rebuilds.
toolchain=$(arm-linux-gnueabi-gcc --version)
libc_input=$({ cat /project/userspace/build-busybox.sh "$musl_source/.source-commit"; printf '%s\n' "$toolchain"; } | sha256sum | cut -d' ' -f1)
if [[ ! -f "$musl_prefix/lib/libc.a" || $(cat "$runtime/.musl-input" 2>/dev/null || true) != "$libc_input" ]]; then
    (cd "$musl_objects"
     "$musl_source/configure" --target=arm-linux-musleabi --prefix="$musl_prefix" --disable-shared --enable-wrapper=gcc CC=arm-linux-gnueabi-gcc CROSS_COMPILE=arm-linux-gnueabi- CFLAGS='-Os -march=armv5te -marm -mfloat-abi=soft'
     make -j"${JOBS:-4}"
     make install)
    printf '%s\n' "$libc_input" > "$runtime/.musl-input"
fi
busybox_input=$(cat /project/userspace/busybox.config /project/userspace/build-busybox.sh "$busybox_source/.source-commit" "$runtime/.musl-input" | sha256sum | cut -d' ' -f1)
if [[ ! -f /work/initramfs/busybox || $(cat "$runtime/.busybox-input" 2>/dev/null || true) != "$busybox_input" ]]; then
    # Use the pinned kernel's exported UAPI with musl, never glibc headers.
    # BusyBox's older allnoconfig resets even explicit fragment booleans.
    # Start with its complete disabled config, merge, then resolve dependencies.
    make -C "$busybox_source" O="$busybox_objects" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- allnoconfig
    python3 - "$busybox_objects/.config" <<'PY'
import re
import sys
from pathlib import Path
path = Path(sys.argv[1])
text = path.read_text()
for line in Path('/project/userspace/busybox.config').read_text().splitlines():
    if not line.startswith('CONFIG_'):
        continue
    name, value = line.split('=', 1)
    pattern = rf'^(?:{name}=.*|# {name} is not set)$'
    replacement = f'# {name} is not set' if value == 'n' else line
    if not re.search(pattern, text, re.M):
        raise SystemExit(f'Unknown BusyBox configuration symbol: {name}')
    text = re.sub(pattern, replacement, text, flags=re.M)
path.write_text(text)
PY
    make -C "$busybox_source" O="$busybox_objects" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- oldconfig </dev/null
    for required in STATIC LFS ASH TELNETD; do
        grep -qx "CONFIG_$required=y" "$busybox_objects/.config"
    done
    make -C "$busybox_source" O="$busybox_objects" -j"${JOBS:-4}" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- CC="$musl_prefix/bin/musl-gcc" EXTRA_CFLAGS="-Os -march=armv5te -marm -mfloat-abi=soft -isystem /work/output-$(uname -m)/usr/include" EXTRA_LDFLAGS='-Wl,-z,max-page-size=4096 -Wl,--build-id=none' KBUILD_BUILD_TIMESTAMP='2026-11-12 00:00:00 UTC'
    arm-linux-gnueabi-strip "$busybox_objects/busybox"
    if ! cmp -s "$busybox_objects/busybox" /work/initramfs/busybox; then
        cp --reflink=never --sparse=never "$busybox_objects/busybox" /work/initramfs/busybox
    fi
    printf '%s\n' "$busybox_input" > "$runtime/.busybox-input"
fi
# Export reproducible metadata and exact target executable alongside Image.
for pair in 'busybox busybox' '.config busybox.config'; do
    read -r name destination <<< "$pair"
    if ! cmp -s "$busybox_objects/$name" "/out/$destination"; then
        cp --reflink=never --sparse=never "$busybox_objects/$name" "/out/$destination"
    fi
done
