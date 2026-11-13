// SPDX-License-Identifier: GPL-2.0-only
/*
 * RM-443 CBUS and watchdog ownership, recovered from this board's firmware.
 * CBUS serial clock/divider, PMIC rails, backup calendar and SoC PLLs are left
 * untouched. The separate 10-second ASIC watchdog cannot be disabled once
 * started; AVilma watchdog register 0x17 is a seconds field, zero powers off.
 */
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/spinlock.h>
#include <linux/watchdog.h>
#include <asm/cacheflush.h>
#include <asm/irqflags.h>
#include "rm443_power.h"
#include "rm443_boot_stage.h"

#define CBUS_ENABLE	0x04
#define CBUS_BUSY	0x08
#define CBUS_READ	0x18
#define CBUS_WRITE	0x1c
#define CBUS_DATA	0x20
#define CBUS_BITS	0x2c
#define CBUS_DIVIDER	0x30
/* Firmware uses 256 busy polls. Allow a larger but still bounded budget here:
 * clocks/timekeeping are not initialized at the first call, so this is a read
 * budget, not a measured microsecond timeout on a physical board. */
#define CBUS_POLL_LIMIT	100000
#define SEM_POLL_LIMIT	100000
#define AVILMA		1
#define BETTY		2
#define PMIC_ID		0x00
#define PMIC_IRQ_MASK	0x02
#define PMIC_STATUS	0x16
#define PMIC_WATCHDOG	0x17
#define PMIC_WDT_MASK	0x3f
#define ASIC_WDT_RELOAD	0x04
#define ASIC_WDT_PERIOD	0x08
#define ASIC_WDT_TICKS	327680
#define DEFAULT_TIMEOUT	32

static void __iomem *cbus, *semaphore, *asic_wdt, *reset_status, *reset_control;
static DEFINE_RAW_SPINLOCK(cbus_lock);
static u16 avilma_id, betty_id, boot_pmic_status, boot_pmic_mask;
static u32 boot_reset_status, boot_cbus_divider, transfers, errors;
static int last_error;
static bool power_ready;
static u8 *reboot_reason;
static struct watchdog_device rm443_wdd;

static int cbus_wait_idle(void)
{
	unsigned int i;
	for (i = 0; i < CBUS_POLL_LIMIT; ++i) {
		if (!readl(cbus + CBUS_BUSY))
			return 0;
		cpu_relax();
	}
	return -ETIMEDOUT;
}

static int cbus_acquire(void)
{
	unsigned int i;
	int error;
	/* This hardware semaphore also arbitrates with the DSP. Its successful
	 * read consumes a token; only its owner writes -1 to release it. */
	for (i = 0; i < SEM_POLL_LIMIT; ++i) {
		if ((s32)readl(semaphore) > 0) {
			error = cbus_wait_idle();
			if (error)
				writel(U32_MAX, semaphore);
			return error;
		}
		cpu_relax();
	}
	return -EBUSY;
}

/* Caller owns cbus_lock and the hardware semaphore. */
static int cbus_transfer(unsigned int chip, unsigned int reg, u16 *value,
			 bool write)
{
	int error;
	writel(1, cbus + CBUS_ENABLE);
	writel(15, cbus + CBUS_BITS);
	if (write) {
		writel(*value, cbus + CBUS_DATA);
		writel((chip << 6) | reg, cbus + CBUS_WRITE);
	} else {
		writel((chip << 6) | reg | 0x20, cbus + CBUS_READ);
	}
	error = cbus_wait_idle();
	if (!error) {
		if (!write)
			*value = readl(cbus + CBUS_DATA);
		++transfers;
	}
	writel(0, cbus + CBUS_ENABLE);
	return error;
}

static void cbus_record_error(int error)
{
	last_error = error;
	if (error)
		++errors;
}

int rm443_cbus_read(unsigned int chip, unsigned int reg, u16 *value)
{
	unsigned long flags;
	int error;
	if (!cbus || !semaphore)
		return -ENODEV;
	if (chip > 7 || reg > 31 || !value)
		return -EINVAL;
	raw_spin_lock_irqsave(&cbus_lock, flags);
	error = cbus_acquire();
	if (!error) {
		error = cbus_transfer(chip, reg, value, false);
		writel(U32_MAX, semaphore);
	}
	cbus_record_error(error);
	raw_spin_unlock_irqrestore(&cbus_lock, flags);
	if (error)
		pr_err_ratelimited("RM-443 CBUS: read chip%u reg%02x failed: %d\n",
				   chip, reg, error);
	return error;
}

int rm443_cbus_update(unsigned int chip, unsigned int reg,
			    u16 mask, u16 value)
{
	unsigned long flags;
	u16 old_value;
	int error;
	if (!cbus || !semaphore)
		return -ENODEV;
	if (chip > 7 || reg > 31)
		return -EINVAL;
	raw_spin_lock_irqsave(&cbus_lock, flags);
	error = cbus_acquire();
	if (!error) {
		error = cbus_transfer(chip, reg, &old_value, false);
		if (!error) {
			old_value = (old_value & ~mask) | (value & mask);
			error = cbus_transfer(chip, reg, &old_value, true);
		}
		writel(U32_MAX, semaphore);
	}
	cbus_record_error(error);
	raw_spin_unlock_irqrestore(&cbus_lock, flags);
	if (error)
		pr_err_ratelimited("RM-443 CBUS: update chip%u reg%02x failed: %d\n",
				   chip, reg, error);
	return error;
}

static int rm443_feed(unsigned int timeout)
{
	int error;
	/* watchdog core ignores max_timeout when max_hw_heartbeat_ms is set.
	 * Reject before masking: 64 would otherwise become PMIC power-off zero. */
	if (!timeout || timeout > PMIC_WDT_MASK)
		return -EINVAL;
	error = rm443_cbus_update(AVILMA, PMIC_WATCHDOG, PMIC_WDT_MASK, timeout);
	/* A failed PMIC transaction is not a successful dual-watchdog ping.
	 * Leave the ASIC deadline running so a bus failure cannot be hidden. */
	if (!error)
		writel(0, asic_wdt + ASIC_WDT_RELOAD);
	return error;
}

int rm443_power_refresh(void)
{
	if (!power_ready)
		return -ENODEV;
	return rm443_feed(rm443_wdd.timeout ?: DEFAULT_TIMEOUT);
}

int __init rm443_power_early_init(void)
{
	struct device_node *node;
	int error;

	node = of_find_compatible_node(NULL, NULL, "nokia,rm443-power");
	if (!node)
		return -ENODEV;
	cbus = of_iomap(node, 0);
	semaphore = of_iomap(node, 1);
	asic_wdt = of_iomap(node, 2);
	reset_status = of_iomap(node, 3);
	reset_control = of_iomap(node, 4);
	of_node_put(node);
	if (!cbus || !semaphore || !asic_wdt || !reset_status || !reset_control)
		return -ENOMEM;
	boot_reset_status = readl(reset_status);
	boot_cbus_divider = readl(cbus + CBUS_DIVIDER);
	/* Service an already-running ASIC watchdog before any bus transactions.
	 * This does not invent an enable/disable command or change its period. */
	writel(0, asic_wdt + ASIC_WDT_RELOAD);
	error = rm443_cbus_read(AVILMA, PMIC_ID, &avilma_id);
	if (!error)
		error = rm443_cbus_read(BETTY, PMIC_ID, &betty_id);
	if (error)
		return error;
	/* Accept the RM-443 chip families, not a particular firmware/revision. */
	if ((avilma_id & 0xf80) != 0x280 || (betty_id & 0xf00) != 0xb00) {
		pr_err("RM-443: unsupported PMIC IDs %04x/%04x\n", avilma_id, betty_id);
		return -ENODEV;
	}
	error = rm443_feed(DEFAULT_TIMEOUT);
	if (error)
		return error;
	/* Firmware's only established ASIC timeout setting: ten 32768-Hz seconds. */
	writel(ASIC_WDT_TICKS, asic_wdt + ASIC_WDT_PERIOD);
	error = rm443_cbus_read(AVILMA, PMIC_STATUS, &boot_pmic_status);
	if (!error)
		error = rm443_cbus_read(AVILMA, PMIC_IRQ_MASK, &boot_pmic_mask);
	if (!error)
		/* Mask IRQ delivery without clearing event latches or touching RTC. */
		error = rm443_cbus_update(AVILMA, PMIC_IRQ_MASK, 0xffff, 0xffff);
	if (error)
		return error;
	power_ready = true;
	pr_info("RM-443 power: AVilma %04x Betty %04x, watchdogs ASIC 10s / PMIC 32s\n",
		avilma_id, betty_id);
	return 0;
}

static int rm443_wdt_ping(struct watchdog_device *wdd)
{
	return rm443_feed(wdd->timeout);
}

static int rm443_wdt_set_timeout(struct watchdog_device *wdd, unsigned int timeout)
{
	int error = rm443_feed(timeout);
	if (!error)
		wdd->timeout = timeout;
	return error;
}

static const struct watchdog_info rm443_wdt_info = {
	.identity = "RAPS and AVilma",
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING,
};

static const struct watchdog_ops rm443_wdt_ops = {
	.owner = THIS_MODULE,
	.start = rm443_wdt_ping,
	.ping = rm443_wdt_ping,
	.set_timeout = rm443_wdt_set_timeout,
};

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buffer)
{
	u16 pmic_status = 0, watchdog_raw = 0, rtc3 = 0, rtc4 = 0;
	int status_error = rm443_cbus_read(AVILMA, PMIC_STATUS, &pmic_status);
	int watchdog_error = rm443_cbus_read(AVILMA, PMIC_WATCHDOG, &watchdog_raw);
	int rtc3_error = rm443_cbus_read(AVILMA, 3, &rtc3);
	int rtc4_error = rm443_cbus_read(AVILMA, 4, &rtc4);
	return sysfs_emit(buffer,
		"AVilma=%04x Betty=%04x\n"
		"reset_status_raw=%08x\n"
		"pmic_status_raw=%04x error=%d boot=%04x\n"
		"watchdog_raw=%04x error=%d timeout=%u ASIC=10s nowayout=1\n"
		"cbus_transfers=%u errors=%u last_error=%d\n"
		"boot_irq_mask=%04x cbus_divider=%08x\n"
		"rtc3_raw=%04x error=%d rtc4_raw=%04x error=%d\n",
		avilma_id, betty_id, boot_reset_status,
		pmic_status, status_error, boot_pmic_status,
		watchdog_raw, watchdog_error, rm443_wdd.timeout,
		transfers, errors, last_error, boot_pmic_mask, boot_cbus_divider,
		rtc3, rtc3_error, rtc4, rtc4_error);
}
static DEVICE_ATTR_RO(status);
static struct attribute *rm443_power_attrs[] = {
	&dev_attr_status.attr,
	NULL,
};
static const struct attribute_group rm443_power_group = {
	.attrs = rm443_power_attrs,
};

static void rm443_reboot_state_init(struct device *dev)
{
	struct device_node *node;
	struct reserved_mem *memory;
	u32 offset;
	u8 *page;

	node = of_find_compatible_node(NULL, NULL, "nokia,rm443-reboot-state");
	if (!node) {
		dev_warn(dev, "firmware warm-reset state not supplied\n");
		return;
	}
	memory = of_reserved_mem_lookup(node);
	if (!memory || memory->size != 4096 || memory->base < 0x10000000 ||
	    memory->base > 0x11fff000 ||
	    of_property_read_u32(node, "nokia,reset-reason-offset", &offset) ||
	    (offset & 3) || offset > 4096 - 0x128) {
		dev_warn(dev, "invalid firmware warm-reset reservation\n");
		of_node_put(node);
		return;
	}
	of_node_put(node);
	/* Keep the ordinary cached linear mapping of this reserved RAM page. */
	page = devm_memremap(dev, memory->base, memory->size, MEMREMAP_WB);
	if (IS_ERR(page)) {
		dev_warn(dev, "cannot map firmware warm-reset state\n");
		return;
	}
	if (READ_ONCE(*(u32 *)(page + offset + 0x124)) != 0x0a9ef561) {
		dev_warn(dev, "firmware warm-reset state lost its boot token\n");
		return;
	}
	reboot_reason = page + offset;
	dev_info(dev, "retained firmware reset page at %pa, reason offset %x\n",
		 &memory->base, offset);
}

static int rm443_shutdown_notify(struct notifier_block *nb,
				 unsigned long action, void *unused)
{
	if (action == SYS_RESTART || action == SYS_POWER_OFF) {
		rm443_boot_stage(action == SYS_RESTART ? 0x90 : 0x80);
		/* Device shutdown may sleep; extend both inherited deadlines first. */
		if (rm443_feed(DEFAULT_TIMEOUT))
			pr_emerg("RM-443: shutdown watchdog refresh failed\n");
	}
	return NOTIFY_DONE;
}

static struct notifier_block rm443_shutdown_notifier = {
	.notifier_call = rm443_shutdown_notify,
};

static int rm443_shutdown_prepared(struct sys_off_data *data)
{
	rm443_boot_stage(data->cb_data ? 0x91 : 0x81);
	if (rm443_feed(DEFAULT_TIMEOUT))
		pr_emerg("RM-443: final watchdog refresh failed\n");
	return NOTIFY_DONE;
}

static int rm443_power_off(struct sys_off_data *data)
{
	int error;
	local_irq_disable();
	local_fiq_disable();
	rm443_boot_stage(0x82);
	/* Match firmware poweroff: refresh ASIC before the AVilma zero write. */
	writel(0, asic_wdt + ASIC_WDT_RELOAD);
	error = rm443_cbus_update(AVILMA, PMIC_WATCHDOG, PMIC_WDT_MASK, 0);
	if (error)
		pr_emerg("RM-443: poweroff watchdog write failed: %d\n", error);
	mdelay(1000);
	rm443_boot_stage(0x8f);
	pr_emerg("RM-443: poweroff command did not stop the CPU\n");
	return NOTIFY_STOP;
}

static int rm443_restart(struct sys_off_data *data)
{
	local_irq_disable();
	local_fiq_disable();
	rm443_boot_stage(0x92);
	/* Reset does not grant a fresh external-PMIC deadline. */
	writel(0, asic_wdt + ASIC_WDT_RELOAD);
	if (rm443_feed(DEFAULT_TIMEOUT))
		pr_emerg("RM-443: restart PMIC refresh failed\n");
	if (reboot_reason) {
		/* Both audited firmwares retain this page across warm reset. Their
		 * startup powers off when the byte at reason+2 reaches six. Preserve
		 * the boot-policy token/other fields; mark an ordinary user restart. */
		WRITE_ONCE(reboot_reason[0], 3);
		WRITE_ONCE(reboot_reason[2], 0);
	} else {
		pr_emerg("RM-443: restarting without verified firmware reset state\n");
	}
	/* Firmware masks IRQ/FIQ, cleans/invalidates D-cache and drains writes
	 * before the reset command. Flush the retained state as well. */
	flush_cache_all();
	dsb();
	writel(2, reset_control);
	dsb();
	mdelay(1000);
	rm443_boot_stage(0x9f);
	pr_emerg("RM-443: reset command did not stop the CPU\n");
	return NOTIFY_STOP;
}

static int rm443_power_probe(struct platform_device *pdev)
{
	int error;
	if (!power_ready)
		return -ENODEV;
	rm443_reboot_state_init(&pdev->dev);
	rm443_wdd.info = &rm443_wdt_info;
	rm443_wdd.ops = &rm443_wdt_ops;
	rm443_wdd.parent = &pdev->dev;
	rm443_wdd.timeout = DEFAULT_TIMEOUT;
	rm443_wdd.min_timeout = 1;
	rm443_wdd.max_timeout = 63;
	rm443_wdd.max_hw_heartbeat_ms = 10000;
	watchdog_set_nowayout(&rm443_wdd, true);
	set_bit(WDOG_HW_RUNNING, &rm443_wdd.status);
	error = rm443_feed(rm443_wdd.timeout);
	if (error)
		return error;
	error = devm_watchdog_register_device(&pdev->dev, &rm443_wdd);
	if (error)
		return error;
	error = devm_device_add_group(&pdev->dev, &rm443_power_group);
	if (error)
		return error;
	error = devm_register_reboot_notifier(&pdev->dev, &rm443_shutdown_notifier);
	if (error)
		return error;
	error = devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_POWER_OFF_PREPARE,
		SYS_OFF_PRIO_DEFAULT, rm443_shutdown_prepared, NULL);
	if (error)
		return error;
	error = devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_RESTART_PREPARE,
		SYS_OFF_PRIO_DEFAULT, rm443_shutdown_prepared, (void *)1);
	if (error)
		return error;
	error = devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_POWER_OFF,
		SYS_OFF_PRIO_DEFAULT, rm443_power_off, NULL);
	if (error)
		return error;
	error = devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_RESTART,
		SYS_OFF_PRIO_DEFAULT, rm443_restart, NULL);
	if (error)
		return error;
	error = rm443_power_key_register(&pdev->dev);
	if (error)
		return error;
	dev_info(&pdev->dev, "dual watchdog /dev/watchdog%u, kernel handover then userspace keepalive\n",
		 rm443_wdd.id);
	return 0;
}

static const struct of_device_id rm443_power_match[] = {
	{ .compatible = "nokia,rm443-power" },
	{}
};

static struct platform_driver rm443_power_driver = {
	.probe = rm443_power_probe,
	.driver = { .name = "rm443-power", .of_match_table = rm443_power_match },
};
builtin_platform_driver(rm443_power_driver);
