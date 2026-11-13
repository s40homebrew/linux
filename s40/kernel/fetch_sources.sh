#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
source_dir=${UPSTREAM_DIR:-$here/../upstream}
mkdir -p "$source_dir"

clone_source() {
    local path=$1 url=$2 revision=$3 branch=${4:-}
    if [[ ! -d "$path/.git" ]]; then
        [[ ! -e "$path" ]] || { echo "Refusing to replace $path" >&2; exit 1; }
        if [[ -n $branch ]]; then
            git clone --no-checkout --depth 1 --branch "$branch" "$url" "$path"
        else
            git clone --no-checkout "$url" "$path"
        fi
    fi
    # No checkout: Linux has case-colliding filenames on common macOS volumes.
    if ! git -C "$path" cat-file -e "$revision^{commit}" 2>/dev/null; then
        git -C "$path" fetch --depth 1 origin "$revision"
    fi
    git -C "$path" cat-file -e "$revision^{commit}"
}

kernel=${KERNEL_SOURCE:-$here/../..}
git -C "$kernel" cat-file -e adc218676eef25575469234709c2d87185ca223a^{commit}
clone_source "${BUSYBOX_SOURCE:-$source_dir/busybox}" https://git.busybox.net/busybox fc71374dfccd46448c62947269a35f1420d7ee28
clone_source "${MUSL_SOURCE:-$source_dir/musl}" https://git.musl-libc.org/git/musl 7813e9119eb5fe681b6750b8a033a0490bca0941
