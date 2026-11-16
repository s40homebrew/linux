# SPDX-License-Identifier: MIT
# shellcheck shell=bash disable=SC2034
ARTIFACT=Linux6303MmuProbe
APP_SOURCES=("$EXAMPLE_DIR/native/mmu_probe.c" "$EXAMPLE_DIR/../boot_probe/native/capture.c")
APP_FLAGS=(-Wall -Wextra -Werror "-I$EXAMPLE_DIR/../boot_probe/native" "-ffile-prefix-map=$EXAMPLE_DIR/..=.")
