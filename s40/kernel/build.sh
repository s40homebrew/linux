#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
source_dir=${UPSTREAM_DIR:-$here/../upstream}
bash "$here/fetch_sources.sh"
kernel=$(cd "${KERNEL_SOURCE:-$here/../..}" && pwd)
busybox=$(cd "${BUSYBOX_SOURCE:-$source_dir/busybox}" && pwd)
musl=$(cd "${MUSL_SOURCE:-$source_dir/musl}" && pwd)
command -v docker >/dev/null
mkdir -p "$here/build"
export BUILDX_CONFIG="${BUILDX_CONFIG:-$here/build/buildx}"
docker build -t s40-linux:6.12 "$here"
# Export Git objects onto a case-sensitive Linux volume; sources stay untouched.
docker run --rm --network none --mount "type=bind,source=$kernel,target=/reference,readonly" --mount "type=bind,source=$busybox,target=/busybox-reference,readonly" --mount "type=bind,source=$musl,target=/musl-reference,readonly" --mount "type=bind,source=$here,target=/project,readonly" --mount "type=bind,source=$here/build,target=/out" --mount "type=volume,source=${BUILD_VOLUME:-s40-linux612-build},target=/work" -e "JOBS=${JOBS:-4}" s40-linux:6.12 bash /project/build-in-container.sh
