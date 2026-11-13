// SPDX-License-Identifier: GPL-2.0-only
/* The separate power key is AVilma status bit5, not a matrix GPIO.
 * Read-only polling leaves PMIC IRQ/RTC event ownership with power.c.
 */
#include <linux/input.h>
#include <linux/slab.h>
#include "rm443_power.h"

struct rm443_power_key {
	bool last, stable;
	unsigned int samples;
};

static void rm443_power_key_poll(struct input_dev *input)
{
	struct rm443_power_key *key = input_get_drvdata(input);
	u16 status;
	bool pressed;

	if (rm443_cbus_read(1, 0x16, &status)) {
		/* A failed transfer is not a released key. Restart the debounce
		 * sequence after recovery instead of synthesizing an input event. */
		key->samples = 0;
		return;
	}
	pressed = !(status & BIT(5));
	if (key->samples && pressed == key->last) {
		if (pressed != key->stable) {
			input_report_key(input, KEY_POWER, pressed);
			input_sync(input);
			key->stable = pressed;
		}
	} else {
		key->samples = 1;
	}
	key->last = pressed;
}

int rm443_power_key_register(struct device *dev)
{
	struct input_dev *input;
	struct rm443_power_key *key;
	int error;

	key = devm_kzalloc(dev, sizeof(*key), GFP_KERNEL);
	if (!key)
		return -ENOMEM;
	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	input->name = "AVilma power key";
	input->phys = "rm443-power/input0";
	input->id.bustype = BUS_HOST;
	input_set_drvdata(input, key);
	input_set_capability(input, EV_KEY, KEY_POWER);
	error = input_setup_polling(input, rm443_power_key_poll);
	if (error)
		return error;
	input_set_poll_interval(input, 100);
	input_set_min_poll_interval(input, 100);
	input_set_max_poll_interval(input, 500);
	return input_register_device(input);
}
