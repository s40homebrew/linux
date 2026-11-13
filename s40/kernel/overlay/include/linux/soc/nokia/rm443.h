/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_NOKIA_RM443_H
#define _LINUX_SOC_NOKIA_RM443_H

void rm443_boot_stage(unsigned int code);

/* Stop the early boot marker before the DRM driver starts using the LCD FIFO. */
void rm443_lcd_takeover(void);

#endif
