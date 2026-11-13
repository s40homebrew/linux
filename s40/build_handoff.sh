#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
PROJECT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
if [[ -z ${LLVM:-} ]]; then
    if command -v llvm-objcopy >/dev/null 2>&1; then
        LLVM=$(dirname "$(command -v llvm-objcopy)")
    else
        LLVM=/opt/homebrew/opt/llvm/bin
    fi
fi
CC=${CC:-$LLVM/clang}
LD=${LD:-$(command -v ld.lld)}
BUILD_DIR="$PROJECT_DIR/build/handoff"
mkdir -p "$BUILD_DIR"
"$CC" -target arm-none-eabi -march=armv5te -marm -c "$PROJECT_DIR/handoff/arm926_enter.S" -o "$BUILD_DIR/enter.o"
"$LD" -T "$PROJECT_DIR/handoff/entry.ld" -o "$BUILD_DIR/enter.elf" "$BUILD_DIR/enter.o"
"$LLVM/llvm-objcopy" -O binary "$BUILD_DIR/enter.elf" "$BUILD_DIR/enter.bin"
"$LLVM/llvm-objdump" -d "$BUILD_DIR/enter.elf" > "$BUILD_DIR/enter.disasm.txt"
"$LLVM/llvm-nm" --defined-only "$BUILD_DIR/enter.elf" > "$BUILD_DIR/enter.symbols.txt"
python3 "$PROJECT_DIR/tools/handoff_layout.py" "$BUILD_DIR"
