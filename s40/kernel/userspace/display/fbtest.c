/* SPDX-License-Identifier: MIT */
/* Exercise the real fbdev mapping, including its deferred damage updates. */
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/fb.h>

static uint32_t channel_mask(struct fb_bitfield field)
{
	return field.length ? ((1u << field.length) - 1) << field.offset : 0;
}

static int valid_channel(struct fb_bitfield field, unsigned int bpp)
{
	return !field.msb_right && field.length <= 8 && field.offset <= bpp &&
		field.length <= bpp - field.offset;
}

static uint32_t channel(unsigned int value, struct fb_bitfield field)
{
	if (!field.length)
		return 0;
	return ((value * ((1u << field.length) - 1) + 127) / 255) << field.offset;
}

int main(int argc, char **argv)
{
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	struct display_vt vt = { .control = -1, .active = -1 };
	const char *device = "/dev/fb0";
	unsigned int seconds = 5, bytes, x, y;
	uint64_t end, first, row_bytes;
	uint32_t red, green, blue, alpha;
	unsigned char *map = MAP_FAILED, *saved = NULL;
	size_t map_len = 0;
	int fd = -1, info = 0, result = 1, i, copied = 0;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--info"))
			info = 1;
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			if (display_seconds(argv[++i], &seconds))
				goto usage;
		} else if (!strcmp(argv[i], "--device") && i + 1 < argc)
			device = argv[++i];
		else
			goto usage;
	}
	fd = open(device, (info ? O_RDONLY : O_RDWR) | O_CLOEXEC);
	if (fd < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &var)) {
		perror(device);
		goto out;
	}
	printf("%s: %.16s, %ux%u, %u bpp, stride %u, memory %u bytes\n",
	       device, fix.id, var.xres, var.yres, var.bits_per_pixel,
	       fix.line_length, fix.smem_len);
	printf("RGB offsets/lengths %u/%u %u/%u %u/%u; virtual %ux%u, offset %u,%u\n",
	       var.red.offset, var.red.length, var.green.offset, var.green.length,
	       var.blue.offset, var.blue.length, var.xres_virtual, var.yres_virtual,
	       var.xoffset, var.yoffset);
	if (info) {
		result = 0;
		goto out;
	}
	if (fix.type != FB_TYPE_PACKED_PIXELS || fix.visual != FB_VISUAL_TRUECOLOR ||
	    (var.bits_per_pixel != 16 && var.bits_per_pixel != 32) ||
	    var.nonstd || var.grayscale || var.rotate || var.xres < 2 || var.yres < 2 ||
	    var.xres > 4096 || var.yres > 4096 ||
	    !valid_channel(var.red, var.bits_per_pixel) ||
	    !valid_channel(var.green, var.bits_per_pixel) ||
	    !valid_channel(var.blue, var.bits_per_pixel) ||
	    !valid_channel(var.transp, var.bits_per_pixel)) {
		fprintf(stderr, "Unsupported framebuffer pixel layout.\n");
		goto out;
	}
	red = channel_mask(var.red);
	green = channel_mask(var.green);
	blue = channel_mask(var.blue);
	alpha = channel_mask(var.transp);
	if (!red || !green || !blue || (red & green) || (red & blue) ||
	    (green & blue) || ((red | green | blue) & alpha)) {
		fprintf(stderr, "Invalid framebuffer color fields.\n");
		goto out;
	}
	bytes = var.bits_per_pixel / 8;
	row_bytes = (uint64_t)var.xres * bytes;
	first = (uint64_t)var.yoffset * fix.line_length + (uint64_t)var.xoffset * bytes;
	end = first + (uint64_t)(var.yres - 1) * fix.line_length + row_bytes;
	if ((uint64_t)var.xoffset + var.xres > var.xres_virtual ||
	    (uint64_t)var.yoffset + var.yres > var.yres_virtual ||
	    ((uint64_t)var.xoffset + var.xres) * bytes > fix.line_length ||
	    end > fix.smem_len || end > SIZE_MAX) {
		fprintf(stderr, "Framebuffer dimensions exceed its backing memory.\n");
		goto out;
	}
	map_len = (size_t)end;
	map = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	saved = malloc((size_t)row_bytes * var.yres);
	if (map == MAP_FAILED || !saved) {
		perror("map/save framebuffer");
		goto out;
	}
	if (display_signals() || display_vt_graphics(&vt))
		goto out;
	for (y = 0; y < var.yres; y++)
		memcpy(saved + y * (size_t)row_bytes, map + first + y * fix.line_length,
		       (size_t)row_bytes);
	copied = 1;
	for (y = 0; y < var.yres && !display_stop; y++) {
		for (x = 0; x < var.xres; x++) {
			uint32_t rgb = display_rgb(x, y, var.xres, var.yres);
			uint32_t pixel = channel((rgb >> 16) & 255, var.red) |
				channel((rgb >> 8) & 255, var.green) |
				channel(rgb & 255, var.blue) | alpha;
			memcpy(map + first + y * fix.line_length + x * bytes, &pixel, bytes);
		}
	}
	printf("Pattern displayed for %u seconds.\n", seconds);
	fflush(stdout);
	display_wait(seconds);
	result = display_stop ? 128 + display_stop : 0;
out:
	if (copied)
		for (y = 0; y < var.yres; y++)
			memcpy(map + first + y * fix.line_length, saved + y * (size_t)row_bytes,
			       (size_t)row_bytes);
	display_vt_restore(&vt);
	free(saved);
	if (map != MAP_FAILED)
		munmap(map, map_len);
	if (fd >= 0)
		close(fd);
	return result;
usage:
	fprintf(stderr, "usage: fbtest [--info] [--seconds 1..300] [--device /dev/fb0]\n");
	return 2;
}
