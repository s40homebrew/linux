// SPDX-License-Identifier: GPL-2.0-only
/* RM-443 GPIO matrix: six active-low rows GPIO32..37, columns GPIO39..42.
 * Recovered from the handset schematic and firmware scanner. Polling avoids
 * depending on the still-unported GPIO IRQ domain; no firmware calls remain.
 */
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define ROW_MASK 0x3f
#define COL_MASK 0x780

struct rm443_keys {
	void __iomem *io;
	u32 last_sample, stable;
};

static const unsigned short keymap[24] = {
	KEY_F1, KEY_LEFT, KEY_F2, KEY_RIGHT,
	KEY_SEND, KEY_UP, KEY_END, KEY_DOWN,
	KEY_1, KEY_2, KEY_3, KEY_ENTER,
	KEY_4, KEY_5, KEY_6, KEY_VOLUMEUP,
	KEY_7, KEY_8, KEY_9, KEY_VOLUMEDOWN,
	KEY_NUMERIC_STAR, KEY_0, KEY_NUMERIC_POUND, KEY_RESERVED,
};

static void rm443_keys_poll(struct input_dev *input)
{
	struct rm443_keys *keys = input_get_drvdata(input);
	u32 sample = 0, changed;
	unsigned int col, r, i;

	for (col = 0; col < 4; col++) {
		/* SET/CLEAR aliases affect only our columns, preserving other pins. */
		writel(COL_MASK, keys->io + 0x04);
		writel(BIT(col + 7), keys->io + 0x08);
		udelay(2); /* Conservative settling delay; not a measured board value. */
		for (r = 0; r < 6; r++)
			if (!(readl(keys->io + 0xc8) & BIT(r)))
				sample |= BIT(r * 4 + col);
	}
	writel(COL_MASK, keys->io + 0x04);
	writel(ROW_MASK, keys->io + 0xe0);
	/* Require the same complete matrix on two polls before reporting. */
	if (sample == keys->last_sample) {
		changed = sample ^ keys->stable;
		for (i = 0; i < ARRAY_SIZE(keymap); i++)
			if ((changed & BIT(i)) && keymap[i] != KEY_RESERVED)
				input_report_key(input, keymap[i], !!(sample & BIT(i)));
		if (changed)
			input_sync(input);
		keys->stable = sample;
	}
	keys->last_sample = sample;
}

static int rm443_keys_probe(struct platform_device *pdev)
{
	struct input_dev *input;
	struct rm443_keys *keys;
	unsigned int i;
	int error;

	keys = devm_kzalloc(&pdev->dev, sizeof(*keys), GFP_KERNEL);
	if (!keys)
		return -ENOMEM;
	/* Only claim the matrix register slice, not USB pinmux at 20031c68. */
	keys->io = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(keys->io))
		return PTR_ERR(keys->io);
	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return -ENOMEM;
	input->name = "RAPS matrix keypad";
	input->phys = "rm443-keypad/input0";
	input->id.bustype = BUS_HOST;
	input_set_drvdata(input, keys);
	for (i = 0; i < ARRAY_SIZE(keymap); i++)
		if (keymap[i] != KEY_RESERVED)
			input_set_capability(input, EV_KEY, keymap[i]);
	error = input_setup_polling(input, rm443_keys_poll);
	if (error)
		return error;
	input_set_poll_interval(input, 20);
	input_set_min_poll_interval(input, 20);
	input_set_max_poll_interval(input, 100);
	/* Quiesce inherited row interrupts, then configure rows/columns only. */
	writel(ROW_MASK, keys->io + 0xd4);
	writel(COL_MASK, keys->io + 0x04);
	writel(ROW_MASK, keys->io + 0x34);
	writel(COL_MASK, keys->io + 0x38);
	writel(ROW_MASK, keys->io + 0xe0);
	return input_register_device(input);
}

static const struct of_device_id rm443_keys_match[] = {
	{ .compatible = "nokia,rm443-keypad" },
	{}
};

static struct platform_driver rm443_keys_driver = {
	.probe = rm443_keys_probe,
	.driver = { .name = "rm443-keypad", .of_match_table = rm443_keys_match },
};
builtin_platform_driver(rm443_keys_driver);
