// SPDX-License-Identifier: GPL-2.0-only
/* Visible checkpoints before the normal console has probed. */
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/printk.h>
#include <linux/soc/nokia/rm443.h>
#include "rm443_boot_stage.h"

#define RM443_MARKER_READ32(base, offset) readl((void __iomem *)(base) + (offset))
#define RM443_MARKER_WRITE32(base, offset, value) \
	writel((value), (void __iomem *)(base) + (offset))
#include "rm443_boot_marker.h"

static bool lcd_owned;

void rm443_lcd_takeover(void)
{
	/* Single CPU: once this flag is set no checkpoint touches the FIFO.
	 * Checkpoint messages still reach the RAM, USB and framebuffer consoles.
	 */
	WRITE_ONCE(lcd_owned, true);
}

void rm443_boot_stage(unsigned int code)
{
	static void __iomem *base;
	static bool failed;
	unsigned long flags;
	int status;

	pr_info("RM-443 boot checkpoint %02X\n", code);
	if (failed || READ_ONCE(lcd_owned))
		return;
	if (!base) {
		/* The board's map_io installed this page before init_early. ARM
		 * ioremap reuses it, so this first call requires no slab allocator.
		 */
		base = ioremap(0x20022200, 0x20);
		if (!base) {
			failed = true;
			return;
		}
	}
	/* Serialize early checkpoints; DRM owns every LCD write after takeover. */
	local_irq_save(flags);
	status = rm443_boot_marker((void __force *)base, code);
	local_irq_restore(flags);
	if (status) {
		/* One stalled attempt is enough. Diagnostics must not repeatedly
		 * delay watchdog ownership when the physical panel is unavailable.
		 */
		failed = true;
		pr_warn("RM-443: boot marker LCD completion timed out\n");
	}
}
