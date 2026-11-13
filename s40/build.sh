#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
bash "$here/kernel/build.sh"
exec bash "$here/build_boot_probe.sh"
