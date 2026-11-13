/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RAPS_PIXELS_H
#define RAPS_PIXELS_H

#include <linux/types.h>
#include <linux/unaligned.h>

static inline void raps_pixel_rgb888(u8 *dst, u16 rgb565)
{
	u8 red = rgb565 >> 11;
	u8 green = (rgb565 >> 5) & 63;
	u8 blue = rgb565 & 31;

	dst[0] = (red << 3) | (red >> 2);
	dst[1] = (green << 2) | (green >> 4);
	dst[2] = (blue << 3) | (blue >> 2);
}

static inline void raps_pixels_expand(u8 *pixels, unsigned int count)
{
	while (count) {
		u16 pixel;

		--count;
		pixel = get_unaligned_be16(pixels + count * 2);
		raps_pixel_rgb888(pixels + count * 3, pixel);
	}
}

static inline void raps_pixels_rgb_order(u8 *pixels, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; ++i) {
		u8 blue = pixels[i * 3];

		pixels[i * 3] = pixels[i * 3 + 2];
		pixels[i * 3 + 2] = blue;
	}
}

static inline u32 raps_pixel_word(const u8 *pixels, unsigned int bytes)
{
	u32 word = 0;
	unsigned int i;

	if (bytes == 4)
		return get_unaligned_le32(pixels);
	for (i = 0; i < bytes; ++i)
		word |= (u32)pixels[i] << (i * 8);
	return word;
}

#endif
