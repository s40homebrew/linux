# SPDX-License-Identifier: MIT
# shellcheck shell=bash disable=SC2034
ARTIFACT=Linux6303Probe
APP_SOURCES=("$EXAMPLE_DIR/native/probe.c")
APP_FLAGS=(-Wall -Wextra -Werror "-ffile-prefix-map=$EXAMPLE_DIR/..=.")
