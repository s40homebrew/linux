// SPDX-License-Identifier: GPL-2.0-only
/*
 * One reserved, physically contiguous 4 KiB RAM page, explicitly selected by
 * earlycon=rm443ram,mmio32,<physical-address>. This is RAM, never a UART/MMIO
 * register block. ARM FIX_EARLYCON_MEM maps one page; require page alignment.
 * The page must also be reserved-memory in the supplied DT, retaining the
 * normal linear map. Small no-map holes at the bank end make ARM926 Linux
 * discard the final 2 MiB. The reserved linear alias must never be accessed;
 * the loader cleans/invalidates D-cache before this uncached mapping is used.
 *
 * Little-endian header: magic, version, total bytes written, capacity.
 * Bytes start at +16. No persistence across reset is promised by this driver.
 */
#include <linux/console.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/serial_core.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/tty.h>
#include "rm443_inspect.h"

#define RM443_RAM_MAGIC 0x5334304c
#define RM443_RAM_HEADER 16
#define RM443_RAM_CAPACITY (SZ_4K - RM443_RAM_HEADER)

/* ARM's FIX_EARLYCON_MEM_BASE is a permanent fixed address. During
 * early_fixmap_shutdown(), arch/arm/mm/mmu.c copies this IO PTE into the
 * normal page tables; it is not an init-only early_ioremap slot. Keep this
 * uncached mapping for the normal console too. ioremap() rejects mapped RAM
 * with different attributes, so creating another alias would be incorrect. */
static void __iomem *rm443_ram_base;
static phys_addr_t rm443_ram_address;
static DEFINE_RAW_SPINLOCK(rm443_ram_lock);
static struct tty_driver *rm443_tty_driver;
static struct tty_port rm443_tty_port;

#ifdef CONFIG_RM443_INSPECT
phys_addr_t rm443_ram_log_address(void)
{
	return rm443_ram_address;
}

int rm443_ram_log_snapshot(void *buffer, size_t bytes)
{
	unsigned long flags;

	if (!rm443_ram_base || bytes != SZ_4K)
		return -EINVAL;
	/* Use the console's existing uncached alias, never the cached RAM map. */
	raw_spin_lock_irqsave(&rm443_ram_lock, flags);
	memcpy_fromio(buffer, rm443_ram_base, bytes);
	raw_spin_unlock_irqrestore(&rm443_ram_lock, flags);
	return 0;
}
#endif

static void rm443_ram_append(const char *text, unsigned int count)
{
	void __iomem *base = rm443_ram_base;
	unsigned long flags;
	u32 written;

	if (!base)
		return;
	raw_spin_lock_irqsave(&rm443_ram_lock, flags);
	written = readl(base + 8);

	while (count--) {
		writeb(*text++, base + RM443_RAM_HEADER +
		       written % RM443_RAM_CAPACITY);
		/* Publish the count after the corresponding data byte. */
		writel(++written, base + 8);
	}
	raw_spin_unlock_irqrestore(&rm443_ram_lock, flags);
}

static void rm443_ram_write(struct console *con, const char *text,
			    unsigned int count)
{
	rm443_ram_append(text, count);
}

static int __init rm443_ram_setup(struct earlycon_device *dev,
				 const char *options)
{
	void __iomem *base = dev->port.membase;

	if (!base || !dev->port.mapbase || (dev->port.mapbase & (SZ_4K - 1)))
		return -EINVAL;
	/* Reset this boot's log. A reader must capture any old log first. */
	writel(0, base);
	writel(1, base + 4);
	writel(0, base + 8);
	writel(RM443_RAM_CAPACITY, base + 12);
	writel(RM443_RAM_MAGIC, base);
	rm443_ram_base = base;
	rm443_ram_address = dev->port.mapbase;
	dev->con->write = rm443_ram_write;
	return 0;
}

EARLYCON_DECLARE(rm443ram, rm443_ram_setup);

/* Keep the same reserved-page transport after early consoles are retired.
 * ttyRM0 supplies a real /dev/console sink. Keypad input is a separate evdev
 * device; reads from this output-only diagnostic TTY wait normally. */
static int rm443_tty_open(struct tty_struct *tty, struct file *file)
{
	return tty_port_open(&rm443_tty_port, tty, file);
}

static void rm443_tty_close(struct tty_struct *tty, struct file *file)
{
	tty_port_close(&rm443_tty_port, tty, file);
}

static void rm443_tty_hangup(struct tty_struct *tty)
{
	tty_port_hangup(&rm443_tty_port);
}

static ssize_t rm443_tty_write(struct tty_struct *tty, const u8 *buf, size_t count)
{
	rm443_ram_append(buf, count);
	return count;
}

static unsigned int rm443_tty_write_room(struct tty_struct *tty)
{
	return 65536;
}

static const struct tty_operations rm443_tty_ops = {
	.open = rm443_tty_open,
	.close = rm443_tty_close,
	.hangup = rm443_tty_hangup,
	.write = rm443_tty_write,
	.write_room = rm443_tty_write_room,
};

static const struct tty_port_operations rm443_port_ops;

static struct tty_driver *rm443_console_device(struct console *con, int *index)
{
	*index = 0;
	return rm443_tty_driver;
}

static struct console rm443_console = {
	.name = "ttyRM",
	.write = rm443_ram_write,
	.device = rm443_console_device,
	.flags = CON_PRINTBUFFER,
	.index = -1,
};

static int __init rm443_ram_console_init(void)
{
	struct tty_driver *driver;
	int ret;

	if (!rm443_ram_base)
		return -ENODEV;
	driver = tty_alloc_driver(1, TTY_DRIVER_RESET_TERMIOS | TTY_DRIVER_REAL_RAW);
	if (IS_ERR(driver))
		return PTR_ERR(driver);
	tty_port_init(&rm443_tty_port);
	rm443_tty_port.ops = &rm443_port_ops;
	driver->driver_name = "rm443-ram";
	driver->name = "ttyRM";
	driver->type = TTY_DRIVER_TYPE_CONSOLE;
	driver->init_termios = tty_std_termios;
	driver->init_termios.c_oflag = OPOST | ONLCR;
	tty_set_operations(driver, &rm443_tty_ops);
	tty_port_link_device(&rm443_tty_port, driver, 0);
	ret = tty_register_driver(driver);
	if (ret) {
		tty_driver_kref_put(driver);
		tty_port_destroy(&rm443_tty_port);
		return ret;
	}
	rm443_tty_driver = driver;
	register_console(&rm443_console);
	return 0;
}
device_initcall(rm443_ram_console_init);
