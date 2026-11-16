#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
PROJECT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
S40_SDK_ROOT=${S40_SDK_ROOT:-$(CDPATH='' cd -- "$PROJECT_DIR/../.." && pwd)/main}
test -f "$S40_SDK_ROOT/tools/env.sh" || { echo 'Clone the S40 homebrew repository into ../main or set S40_SDK_ROOT to its path.' >&2; exit 1; }
# shellcheck source=/dev/null
source "$S40_SDK_ROOT/tools/env.sh"
SHARED_DIR="$S40_SDK_ROOT/homebrew"
test -f "$PROJECT_DIR/kernel/build/Image" || { echo 'Build the kernel first: bash s40/kernel/build.sh' >&2; exit 1; }
mode=$(python3 "$PROJECT_DIR/tools/boot_build_cache.py" check)
if [[ $mode == cached ]]; then
    printf 'Linux launcher inputs unchanged; keeping existing JAR/JAD.\n'
    exit 0
fi
if [[ $mode == package ]]; then
    python3 "$SHARED_DIR/tools/package_midlet.py" "$SHARED_DIR" "$PROJECT_DIR/boot_probe" "$PROJECT_DIR/build/boot_probe" "$PROJECT_DIR/dist" Nokia6303Linux
    python3 "$PROJECT_DIR/tools/boot_build_cache.py" store
    exit 0
fi
bash "$PROJECT_DIR/build_handoff.sh"
export S40_APP_DIR="$PROJECT_DIR/boot_probe"
export S40_BUILD_DIR="$PROJECT_DIR/build/boot_probe"
export S40_DIST_DIR="$PROJECT_DIR/dist"
# Mark ownership before embedding creates generated files in the build folder.
python3 "$S40_SDK_ROOT/tools/clean_build_outputs.py" java "$SHARED_DIR" "$S40_BUILD_DIR" build/linux6303-boot "$S40_APP_DIR"
python3 "$PROJECT_DIR/tools/embed_payload.py"
bash "$SHARED_DIR/build.sh" linux6303-boot
python3 "$PROJECT_DIR/tools/boot_build_cache.py" store
