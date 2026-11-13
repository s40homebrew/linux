/* SPDX-License-Identifier: MIT */
#ifndef BOOT_FDT_H
#define BOOT_FDT_H
#include <stddef.h>
#include <stdint.h>
#ifndef LINUX6303_DISPLAY_DMA
#define LINUX6303_DISPLAY_DMA 1
#endif
#ifndef LINUX6303_SD_SAFE
#define LINUX6303_SD_SAFE 0
#endif
#ifndef LINUX6303_INSPECT
#define LINUX6303_INSPECT 0
#endif
#ifndef LINUX6303_SDLOG
#define LINUX6303_SDLOG 1
#endif
#ifndef LINUX6303_USB_WAIT_MS
#define LINUX6303_USB_WAIT_MS 0
#endif
#ifndef LINUX6303_SD_POLL
#define LINUX6303_SD_POLL 0
#endif
#ifndef LINUX6303_SD_TRACE
#define LINUX6303_SD_TRACE 0
#endif
#ifndef LINUX6303_NOHLT
#define LINUX6303_NOHLT 0
#endif
#define LINUX6303_OPTION_DMA 1u
#define LINUX6303_OPTION_RGB666 2u
#define LINUX6303_OPTION_RGB888 4u
#define LINUX6303_OPTION_COLOR_MASK 6u
struct rm443_swd_state;
size_t linux6303_fdt(void *buffer, size_t capacity, uint32_t base, uint32_t size);
/* Retain a 64-KiB transition block independently of the advertised RAM end. */
size_t linux6303_fdt_at(void *buffer, size_t capacity, uint32_t base, uint32_t size,
                       uint32_t transition);
/* Preserve the discovered S40 warm-reset page; zero means unavailable. */
size_t linux6303_fdt_with_reboot(void *buffer, size_t capacity, uint32_t base,
                               uint32_t size, uint32_t transition, uint32_t reason);
size_t linux6303_fdt_with_secure_watchdog(void *buffer, size_t capacity,
    uint32_t base, uint32_t size, uint32_t transition, uint32_t reason,
    uint32_t secure, uint32_t work, uint32_t work_bytes);
size_t linux6303_fdt_with_secure_state(void *buffer, size_t capacity,
    uint32_t base, uint32_t size, uint32_t transition, uint32_t reason,
    uint32_t secure, const struct rm443_swd_state *state);
size_t linux6303_fdt_with_options(void *buffer, size_t capacity,
    uint32_t base, uint32_t size, uint32_t transition, uint32_t reason,
    uint32_t secure, const struct rm443_swd_state *state, unsigned options);
#endif
