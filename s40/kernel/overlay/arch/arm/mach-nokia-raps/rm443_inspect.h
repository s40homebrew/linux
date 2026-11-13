/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RM443_INSPECT_H
#define RM443_INSPECT_H
#include <linux/types.h>
phys_addr_t rm443_ram_log_address(void);
int rm443_ram_log_snapshot(void *buffer, size_t bytes);
#endif
