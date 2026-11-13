// SPDX-License-Identifier: GPL-2.0-only
/* RAPS interrupt controller: 226 sources, per-source masking and level IRQs. */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>
#include <linux/string.h>
#include <asm/exception.h>
#include <asm/mach/irq.h>

#define RAPS_IRQ_COUNT		226
#define RAPS_TIMER_IRQ		36
#define RAPS_IRQ_DRAIN_LIMIT	4096
#define RAPS_BANK_ENABLE		0x04
#define RAPS_VECTOR		0x08
#define RAPS_ENABLE		0x0c
#define RAPS_DISABLE		0x10
#define RAPS_SOURCE_SELECT	0x20
#define RAPS_SOURCE_CONFIG	0x24
#define RAPS_FIQ_BANK		0x40
#define RAPS_IRQ_LOCK_COUNT	0x90
#define RAPS_FIQ_LOCK_COUNT	0x9c

static void __iomem *raps_intc;
static struct irq_domain *raps_domain;

static void raps_irq_mask(struct irq_data *data)
{
	writel(data->hwirq, raps_intc + RAPS_DISABLE);
}

static void raps_irq_unmask(struct irq_data *data)
{
	writel(data->hwirq, raps_intc + RAPS_ENABLE);
}

static struct irq_chip raps_irq_chip = {
	.name = "RAPS",
	.irq_mask = raps_irq_mask,
	.irq_unmask = raps_irq_unmask,
};

static int raps_irq_map(struct irq_domain *domain, unsigned int irq,
			irq_hw_number_t hwirq)
{
	if (hwirq >= RAPS_IRQ_COUNT)
		return -EINVAL;
	irq_set_chip_and_handler(irq, &raps_irq_chip, handle_level_irq);
	irq_set_status_flags(irq, IRQ_LEVEL);
	irq_set_noprobe(irq);
	return 0;
}

static const struct irq_domain_ops raps_irq_ops = {
	.map = raps_irq_map,
	.xlate = irq_domain_xlate_onecell,
};

static void raps_irq_quarantine(u32 source, unsigned int count)
{
	unsigned int irq = irq_find_mapping(raps_domain, source);

	/* Called after the device handler returns, without its descriptor lock.
	 * Keep Linux's disabled state consistent with the controller mask. */
	if (irq)
		disable_irq_nosync(irq);
	else
		writel(source, raps_intc + RAPS_DISABLE);
	pr_err("RAPS: masked IRQ source %u (Linux IRQ %u) after %u dispatches\n",
	       source, irq, count);
}

static void __exception_irq_entry raps_handle_irq(struct pt_regs *regs)
{
	/* ARM926 is single-core and hard IRQ entry is not reentrant. Keep the
	 * counters off the IRQ stack; each entry gets a fresh traffic budget. */
	static u16 dispatched[RAPS_IRQ_COUNT];
	u32 source;

	memset(dispatched, 0, sizeof(dispatched));
	while ((source = readl(raps_intc + RAPS_VECTOR)) != U32_MAX) {
		if (source >= RAPS_IRQ_COUNT) {
			pr_err_ratelimited("RAPS: invalid pending vector %u\n", source);
			break;
		}
		if (dispatched[source] > RAPS_IRQ_DRAIN_LIMIT) {
			/* Even a failed hardware mask must not trap this drain. */
			pr_err_ratelimited("RAPS: masked IRQ source %u still pending\n",
					   source);
			break;
		}
		if (source != RAPS_TIMER_IRQ) {
			if (dispatched[source] == RAPS_IRQ_DRAIN_LIMIT) {
				raps_irq_quarantine(source, dispatched[source]);
				dispatched[source]++;
				continue;
			}
			dispatched[source]++;
		}
		/* Level sources are acknowledged by their device handler. Reading
		 * the vector alone does not clear a timer or GPIO event. */
		if (generic_handle_domain_irq(raps_domain, source)) {
			raps_irq_quarantine(source, dispatched[source]);
			dispatched[source] = RAPS_IRQ_DRAIN_LIMIT + 1;
		}
	}
}

static int __init raps_irq_init(struct device_node *node,
			       struct device_node *parent)
{
	unsigned int source;

	if (parent || raps_intc)
		return -EINVAL;
	raps_intc = of_iomap(node, 0);
	if (!raps_intc)
		return -ENOMEM;

	/* Remove S40 routing before Linux unmasks its first source. FIQ stays
	 * disabled; Linux uses IRQ only. Pending device latches are not discarded. */
	writel(0, raps_intc + RAPS_BANK_ENABLE);
	writel(0, raps_intc + RAPS_FIQ_BANK + RAPS_BANK_ENABLE);
	writel(U32_MAX, raps_intc + RAPS_DISABLE);
	writel(U32_MAX, raps_intc + RAPS_FIQ_BANK + RAPS_DISABLE);
	for (source = 0; source < RAPS_IRQ_COUNT; ++source) {
		writel(source, raps_intc + RAPS_SOURCE_SELECT);
		writel(2, raps_intc + RAPS_SOURCE_CONFIG);
	}
	writel(0, raps_intc + RAPS_IRQ_LOCK_COUNT);
	writel(0, raps_intc + RAPS_FIQ_LOCK_COUNT);

	raps_domain = irq_domain_add_linear(node, RAPS_IRQ_COUNT,
					   &raps_irq_ops, NULL);
	if (!raps_domain) {
		iounmap(raps_intc);
		raps_intc = NULL;
		return -ENOMEM;
	}
	set_handle_irq(raps_handle_irq);
	writel(1, raps_intc + RAPS_BANK_ENABLE);
	pr_info("RAPS: %u IRQ sources, inherited routes masked\n", RAPS_IRQ_COUNT);
	return 0;
}

IRQCHIP_DECLARE(rm443_intc, "nokia,rm443-intc", raps_irq_init);
