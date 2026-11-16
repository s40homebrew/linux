#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
PROJECT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
S40_SDK_ROOT=${S40_SDK_ROOT:-$(CDPATH='' cd -- "$PROJECT_DIR/../.." && pwd)/main}
source "$S40_SDK_ROOT/tools/env.sh"
export S40_APP_DIR="$PROJECT_DIR/probe"
export S40_BUILD_DIR="$PROJECT_DIR/build/probe"
export S40_DIST_DIR="$PROJECT_DIR/dist"
exec bash "$S40_SDK_ROOT/homebrew/build.sh" linux6303-probe
