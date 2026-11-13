// SPDX-License-Identifier: GPL-2.0-only
/*
 * RM-443 timekeeping: a latched 32.768 kHz free-running clocksource and
 * high-speed countdown channel 2 (IRQ36). The event clock is supplied by DT.
 * Firmware uses this channel at 9.6 MHz.
 */
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>

#define RAPS_SLOW_TIMERS	0x120
#define RAPS_FAST_TIMERS	0x1a0
#define RAPS_EVENT_COUNT	0x1a8
#define RAPS_EVENT_ADD	0x1c8
#define RAPS_COUNTER	0x490
#define RAPS_LATCH	0x494
#define RAPS_COUNTER_HZ	32768

static void __iomem *raps_timer;
static unsigned long raps_period;

static u64 notrace raps_counter_read(void)
{
	unsigned long flags;
	u32 ticks;

	/* The latch is shared by clocksource and sched_clock reads. RM-443 is
	 * uniprocessor; avoid an IRQ replacing a partially completed sample. */
	raw_local_irq_save(flags);
	writel_relaxed(0, raps_timer + RAPS_LATCH);
	ticks = readl_relaxed(raps_timer + RAPS_COUNTER);
	raw_local_irq_restore(flags);
	return ticks;
}

static u64 raps_clocksource_read(struct clocksource *source)
{
	return raps_counter_read();
}

static struct clocksource raps_clocksource = {
	.name = "raps-32k",
	.rating = 200,
	.read = raps_clocksource_read,
	.mask = CLOCKSOURCE_MASK(32),
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
};

static int raps_event_shutdown(struct clock_event_device *event)
{
	/* The countdown register's zero command cancels and clears its level. */
	writel(0, raps_timer + RAPS_EVENT_COUNT);
	return 0;
}

static int raps_event_next(unsigned long delta, struct clock_event_device *event)
{
	writel(delta, raps_timer + RAPS_EVENT_COUNT);
	return 0;
}

static int raps_event_periodic(struct clock_event_device *event)
{
	writel(raps_period, raps_timer + RAPS_EVENT_COUNT);
	return 0;
}

static struct clock_event_device raps_clockevent = {
	.name = "raps-timer2",
	.rating = 200,
	.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT,
	.set_next_event = raps_event_next,
	.set_state_shutdown = raps_event_shutdown,
	.set_state_oneshot = raps_event_shutdown,
	.set_state_periodic = raps_event_periodic,
	.tick_resume = raps_event_shutdown,
};

static irqreturn_t raps_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *event = dev_id;

	if (clockevent_state_periodic(event))
		/* Add to the previous expiry, as firmware does, so interrupt
		 * service latency does not accumulate into scheduler tick drift. */
		writel(raps_period, raps_timer + RAPS_EVENT_ADD);
	else
		writel(0, raps_timer + RAPS_EVENT_COUNT);
	event->event_handler(event);
	return IRQ_HANDLED;
}

static int __init raps_timer_init(struct device_node *node)
{
	u32 frequency;
	unsigned int index;
	int irq, error;

	if (raps_timer)
		return -EBUSY;
	if (of_property_read_u32(node, "clock-frequency", &frequency) || !frequency)
		return -EINVAL;
	irq = irq_of_parse_and_map(node, 0);
	if (!irq)
		return -EINVAL;
	raps_timer = of_iomap(node, 0);
	if (!raps_timer)
		return -ENOMEM;

	/* All ten countdown sources belonged to S40. Their INTC routes have
	 * already been masked, so retire their pending levels before reuse. */
	for (index = 0; index < 5; ++index) {
		writel(0, raps_timer + RAPS_SLOW_TIMERS + index * 4);
		writel(0, raps_timer + RAPS_FAST_TIMERS + index * 4);
	}
	error = clocksource_register_hz(&raps_clocksource, RAPS_COUNTER_HZ);
	if (error)
		return error;
	sched_clock_register(raps_counter_read, 32, RAPS_COUNTER_HZ);
	raps_period = DIV_ROUND_CLOSEST(frequency, HZ);
	raps_clockevent.cpumask = cpumask_of(0);
	raps_clockevent.irq = irq;
	error = request_irq(irq, raps_timer_interrupt, IRQF_TIMER,
			    "raps-timer2", &raps_clockevent);
	if (error)
		return error;
	/* Signed countdown values, with a ten-microsecond minimum. */
	clockevents_config_and_register(&raps_clockevent, frequency,
				       max_t(u32, frequency / 100000, 1), 0x7fffffff);
	pr_info("RAPS: clocksource %u Hz, clockevent %u Hz IRQ %d\n",
		RAPS_COUNTER_HZ, frequency, irq);
	return 0;
}

TIMER_OF_DECLARE(rm443_timer, "nokia,rm443-timer", raps_timer_init);
