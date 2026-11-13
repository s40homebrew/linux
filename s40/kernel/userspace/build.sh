#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd /project/userspace
output=/work/output-$(uname -m)
runtime=/work/runtime-$(uname -m)
mkdir -p /work/initramfs
bash build-busybox.sh

compiler=arm-linux-gnueabi-gcc
toolchain=$($compiler --version)
common_flags=(-Os -Wall -Wextra -march=armv5te -marm -mfloat-abi=soft -fno-stack-protector -fno-pie -no-pie -static '-Wl,--build-id=none' '-Wl,-z,max-page-size=4096' -isystem "$output/usr/include")
nolibc_flags=(-nostdlib -include /work/source/tools/include/nolibc/nolibc.h -I/work/source/tools/include/nolibc)

build_program() {
    local program=$1 libc=$2
    shift 2
    local destination=/work/initramfs/$program
    local input=/work/initramfs/.$program-input
    local fingerprint
    local sources=() dependencies=("$@" build.sh /work/source/.rm443-source-commit)
    local flags=("${common_flags[@]}") libraries=()
    local source cc=$compiler
    for source in "$@"; do
        [[ $source != *.c ]] || sources+=("$source")
    done
    if [[ $libc == nolibc ]]; then
        dependencies+=(/work/source/tools/include/nolibc/*.h)
        flags+=("${nolibc_flags[@]}")
        libraries+=(-lgcc)
    else
        dependencies+=("$runtime/.musl-input")
        cc=$runtime/musl-install/bin/musl-gcc
    fi
    fingerprint=$({ printf '%s\n' "$toolchain" "${flags[@]}"; cat "${dependencies[@]}"; } | sha256sum | cut -d' ' -f1)
    if [[ -f $destination && $(cat "$input" 2>/dev/null || true) == "$fingerprint" ]]; then
        return
    fi
    printf 'Building %s\n' "$program"
    "$cc" "${flags[@]}" "${sources[@]}" "${libraries[@]}" -o "$destination.new"
    arm-linux-gnueabi-strip "$destination.new"
    if ! cmp -s "$destination.new" "$destination"; then
        mv "$destination.new" "$destination"
    else
        rm "$destination.new"
    fi
    printf '%s\n' "$fingerprint" > "$input"
}

build_program init nolibc init/main.c init/runtime.c init/runtime.h init/usb.c init/usb.h
build_program memcheck nolibc memcheck.c
build_program memread musl memread.c
build_program rmprobe musl rmprobe.c ../overlay/include/uapi/linux/rm443_probe.h
build_program sdlog musl sdlog.c
build_program fbtest musl display/fbtest.c display/common.c display/common.h
build_program drmtest musl display/drmtest.c display/common.c display/common.h
if ! cmp -s root.list /work/initramfs/root.list; then
    cp root.list /work/initramfs/root.list
fi
