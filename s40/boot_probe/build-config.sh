# SPDX-License-Identifier: MIT
# shellcheck shell=bash disable=SC2034
ARTIFACT=Nokia6303Linux
NATIVE_PAYLOAD_BUDGET=1
APP_SOURCES=("$EXAMPLE_DIR/native/boot.c" "$EXAMPLE_DIR/native/boot_fdt.c" "$EXAMPLE_DIR/native/reboot_state.c" "$EXAMPLE_DIR/native/secure_watchdog.c" "$EXAMPLE_DIR/native/capture.c" "$BUILD_DIR/payload.S")
# Conservative SD signalling, without automatic SD writes or request tracing.
APP_FLAGS=(-Wall -Wextra -Werror -DLINUX6303_SD_SAFE=1 -DLINUX6303_INSPECT=1 -DLINUX6303_SDLOG=0 -DLINUX6303_USB_WAIT_MS=15000 -DLINUX6303_SD_POLL=1 -DLINUX6303_SD_TRACE=0 -DLINUX6303_NOHLT=1)
