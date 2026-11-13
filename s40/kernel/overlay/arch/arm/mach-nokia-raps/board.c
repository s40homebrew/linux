// SPDX-License-Identifier: GPL-2.0-only
/* Nokia 6303 classic: Linux owns the CPU, IRQ controller and system timers. */
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/irqflags.h>
#include <linux/sizes.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/processor.h>
#include "rm443_power.h"
#include "rm443_boot_stage.h"

/* init_early precedes slab initialization. Register one static IO page during
 * paging_init, so its later ioremap lookup does not need vmalloc/slab memory.
 */
static struct map_desc rm443_early_io[] __initdata = {
	{
		.virtual = 0xfe022000,
		.pfn = __phys_to_pfn(0x20022000),
		.length = SZ_4K,
		.type = MT_DEVICE,
	},
};

static void __init rm443_map_io(void)
{
	iotable_init(rm443_early_io, ARRAY_SIZE(rm443_early_io));
}

static void __init rm443_dma_stop(void)
{
	void __iomem *base = ioremap(0x20040000, 0x3000);
	unsigned int channel, tries;
	bool stopped = !!base;

	/* Firmware's linear stop and circular-channel setup both write zero
	 * then poll the same command register. Linear stop F037BDF0 also clears
	 * the enable register after command completion. This stops those two known DMA
	 * blocks only; DSP/modem and peripheral-local masters are not covered. */
	if (base) {
		for (channel = 0; channel < 10; ++channel) {
			void __iomem *command = base + (channel < 6 ?
				0xc08 + channel * 0x400 : 0x2440 + (channel - 6) * 4);
			writel(0, command);
			/* Timer initialization has not run: bound by reads rather than
			 * using jiffies or an uncalibrated timed delay. */
			for (tries = 0; tries < 100000 && readl(command); ++tries)
				cpu_relax();
			if (tries == 100000)
				stopped = false;
			else if (channel < 6)
				writel(0, command - 4);
		}
		iounmap(base);
	}
	if (!stopped) {
		rm443_boot_stage(0x5f);
		pr_emerg("RM-443: known DMA engines did not stop; IRQ/FIQ masked\n");
		for (;;)
			cpu_relax();
	}
	pr_info("RM-443: stopped six linear and four circular DMA channels\n");
}

static void __init rm443_init_irq(void)
{
	local_irq_disable();
	local_fiq_disable();
	rm443_boot_stage(0x51);
	if (rm443_power_early_init()) {
		rm443_boot_stage(0x5f);
		pr_emerg("RM-443: power/watchdog handover failed; IRQ/FIQ masked\n");
		for (;;)
			cpu_relax();
	}
	rm443_boot_stage(0x52);
	rm443_dma_stop();
	irqchip_init();
	rm443_boot_stage(0x53);
}

static void __init rm443_init_early(void)
{
	/* ARM setup_arch has established paging before this machine hook. */
	rm443_boot_stage(0x50);
}

static void __init rm443_init_time(void)
{
	timer_probe();
	rm443_boot_stage(0x54);
}

static const char *const rm443_compat[] __initconst = {
	"nokia,rm443",
	"nokia,rm443-linux-lab", /* Older launcher device trees. */
	NULL,
};

DT_MACHINE_START(NOKIA_RM443, "Nokia 6303 classic")
	.map_io = rm443_map_io,
	.init_early = rm443_init_early,
	.init_irq = rm443_init_irq,
	.init_time = rm443_init_time,
	.dt_compat = rm443_compat,
MACHINE_END
