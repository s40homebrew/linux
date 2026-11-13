/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Small inherited-panel diagnostic. No allocation, timer or firmware calls.
 * The controller/panel must already be initialized by S40. Mode bits29:20
 * are retained; no panel power, pinmux or bus-timing setup is attempted.
 * Real firmware F02507A4 uses <=8-byte PIO transfers and waits for bit3
 * between transfers. Bit3 is completion, not FIFO space. Use four bytes per
 * transfer here, so every FIFO write has an independently completed count.
 * The read limit is a finite budget, not a measured physical time limit.
 */
#ifndef RM443_BOOT_MARKER_H
#define RM443_BOOT_MARKER_H

#define RM443_MARKER_WIDTH 64
#define RM443_MARKER_HEIGHT 40

/* Seven-segment A..G, bit0..6. Both stages and the stackless handoff use this
 * exact geometry, so recordings show a consistent two-digit hexadecimal code.
 */
static inline unsigned int rm443_marker_pixel(unsigned int code,
					      unsigned int x, unsigned int y)
{
	static const unsigned char digits[16] = {
		0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07,
		0x7f, 0x6f, 0x77, 0x7c, 0x39, 0x5e, 0x79, 0x71,
	};
	unsigned int mask, segment = 0;

	if (x >= RM443_MARKER_WIDTH || y >= RM443_MARKER_HEIGHT)
		return 0;
	mask = digits[(x < 32 ? code >> 4 : code) & 15];
	x &= 31;
	if (x >= 6 && x <= 25) {
		if (y >= 2 && y <= 5)
			segment = 1;
		else if (y >= 18 && y <= 21)
			segment = 64;
		else if (y >= 34 && y <= 37)
			segment = 8;
	} else if (y >= 6 && y <= 17) {
		if (x >= 2 && x <= 5)
			segment = 32;
		else if (x >= 26 && x <= 29)
			segment = 2;
	} else if (y >= 22 && y <= 33) {
		if (x >= 2 && x <= 5)
			segment = 16;
		else if (x >= 26 && x <= 29)
			segment = 4;
	}
	return mask & segment ? 0xffff : 0;
}

/* Override these for kernel readl/writel or a host-side transfer model. */
#ifndef RM443_MARKER_READ32
#define RM443_MARKER_READ32(base, offset) \
	(*(volatile unsigned int *)((unsigned long)(base) + (offset)))
#define RM443_MARKER_WRITE32(base, offset, value) \
	(*(volatile unsigned int *)((unsigned long)(base) + (offset)) = (value))
#endif

static inline int rm443_marker_ready(void *base)
{
	unsigned int tries;
	for (tries = 0; tries < 1000; ++tries)
		if (RM443_MARKER_READ32(base, 8) & 8)
			return 0;
	return -1;
}

static inline int rm443_marker_transfer(void *base, unsigned int mode,
					unsigned int data, unsigned int count,
					unsigned int value)
{
	if (rm443_marker_ready(base))
		return -1;
	/* Same FIFO reset sequence as F0250840 / first chunk in F02507A4. */
	RM443_MARKER_WRITE32(base, 8, 2);
	RM443_MARKER_WRITE32(base, 8, 1);
	RM443_MARKER_WRITE32(base, 4, mode | data | (count - 1));
	/* Firmware also writes command bytes through a full-word store. */
	RM443_MARKER_WRITE32(base, 16, value);
	return 0;
}

static inline int rm443_boot_marker(void *base, unsigned int code)
{
	unsigned int x, y, pixels;
	unsigned int mode = RM443_MARKER_READ32(base, 4) & 0x3ff00000;

	/* RGB565 and the existing 64x40 top-left window; panel stays awake. */
	if (rm443_marker_transfer(base, mode, 0, 1, 0x3a) ||
	    rm443_marker_transfer(base, mode, 0x40000, 1, 0x55) ||
	    rm443_marker_transfer(base, mode, 0, 1, 0x2a) ||
	    rm443_marker_transfer(base, mode, 0x40000, 4, 0x3f000000) ||
	    rm443_marker_transfer(base, mode, 0, 1, 0x2b) ||
	    rm443_marker_transfer(base, mode, 0x40000, 4, 0x27000000) ||
	    rm443_marker_transfer(base, mode, 0, 1, 0x2c))
		return -1;
	for (y = 0; y < RM443_MARKER_HEIGHT; ++y) {
		for (x = 0; x < RM443_MARKER_WIDTH; x += 2) {
			pixels = rm443_marker_pixel(code, x, y) |
				(rm443_marker_pixel(code, x + 1, y) << 16);
			if (rm443_marker_transfer(base, mode, 0x40000, 4, pixels))
				return -1;
		}
	}
	return rm443_marker_ready(base);
}

#endif
