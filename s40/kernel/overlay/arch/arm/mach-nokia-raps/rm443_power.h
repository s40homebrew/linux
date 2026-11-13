/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RM443_POWER_H
#define RM443_POWER_H

#include <linux/types.h>
struct device;

int rm443_cbus_read(unsigned int chip, unsigned int reg, u16 *value);
int rm443_cbus_update(unsigned int chip, unsigned int reg, u16 mask, u16 value);
int rm443_power_early_init(void);
int rm443_power_refresh(void);
int rm443_power_key_register(struct device *dev);

#endif
