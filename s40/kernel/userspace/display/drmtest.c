/* SPDX-License-Identifier: MIT */
/* Direct KMS test for the phone's single connector and CRTC. No libdrm. */
#define _FILE_OFFSET_BITS 64
#include "common.h"
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define USER_PTR(p) ((uint64_t)(uintptr_t)(p))

static void write_pixel(unsigned char *map, unsigned int pitch,
			unsigned int x, unsigned int y, unsigned int bytes, uint32_t rgb)
{
	uint32_t pixel = rgb;
	if (bytes == 2)
		pixel = ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f);
	memcpy(map + (size_t)y * pitch + x * bytes, &pixel, bytes);
}

static uint64_t monotonic_us(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static int benchmark(int fd, uint32_t fb_id, unsigned char *map,
		     const struct drm_mode_create_dumb *dumb, unsigned int bytes,
		     unsigned int frames)
{
	unsigned int test, frame, x, y;
	for (test = 0; test < 2; test++) {
		struct drm_clip_rect clip = {
			.x1 = test ? 7 : 0, .y1 = test ? 11 : 0,
			.x2 = test ? 38 : dumb->width, .y2 = test ? 30 : dumb->height,
		};
		struct drm_mode_fb_dirty_cmd dirty = {
			.fb_id = fb_id, .num_clips = 1, .clips_ptr = USER_PTR(&clip),
		};
		uint64_t start = monotonic_us(), transfer = 0;
		for (frame = 0; frame < frames && !display_stop; frame++) {
			uint32_t color = frame & 1 ? 0xff8000 : 0x0040ff;
			uint64_t before;
			for (y = clip.y1; y < clip.y2; y++)
				for (x = clip.x1; x < clip.x2; x++)
					write_pixel(map, dumb->pitch, x, y, bytes, color);
			before = monotonic_us();
			if (ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty)) {
				perror("benchmark update");
				return -1;
			}
			transfer += monotonic_us() - before;
		}
		printf("%s: %u updates, %llu us/update end-to-end, %llu us/update submit+transfer\n",
		       test ? "31x19" : "240x320", frame,
		       (unsigned long long)((monotonic_us() - start) / (frame ? frame : 1)),
		       (unsigned long long)(transfer / (frame ? frame : 1)));
	}
	fflush(stdout);
	return 0;
}

int main(int argc, char **argv)
{
	uint32_t connectors[16], crtcs[16], encoders[16];
	struct drm_mode_modeinfo modes[64], mode;
	struct drm_mode_card_res resources = {
		.connector_id_ptr = USER_PTR(connectors), .count_connectors = ARRAY_SIZE(connectors),
		.crtc_id_ptr = USER_PTR(crtcs), .count_crtcs = ARRAY_SIZE(crtcs),
	};
	struct drm_mode_get_connector connector = {0};
	struct drm_mode_get_encoder encoder = {0};
	struct drm_mode_crtc saved = {0}, set = {0};
	struct drm_mode_create_dumb dumb = {0};
	struct drm_mode_map_dumb mapping = {0};
	struct drm_mode_fb_cmd2 fb = {0};
	struct display_vt vt = { .control = -1, .active = -1 };
	const char *device = "/dev/dri/card0";
	unsigned int seconds = 5, bytes = 4, frames = 0, x, y;
	unsigned char *map = MAP_FAILED;
	int fd = -1, result = 1, i, changed = 0, master = 0, info = 0;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--rgb565"))
			bytes = 2;
		else if (!strcmp(argv[i], "--benchmark") && i + 1 < argc) {
			if (display_seconds(argv[++i], &frames))
				goto usage;
		} else if (!strcmp(argv[i], "--info"))
			info = 1;
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			if (display_seconds(argv[++i], &seconds))
				goto usage;
		} else if (!strcmp(argv[i], "--device") && i + 1 < argc)
			device = argv[++i];
		else
			goto usage;
	}
	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0 || ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources)) {
		perror(device);
		goto out;
	}
	/* Restoration below is deliberately limited to a single display pipeline. */
	if (resources.count_connectors != 1 || resources.count_crtcs != 1) {
		fprintf(stderr, "Expected one connector and one CRTC, got %u and %u.\n",
			resources.count_connectors, resources.count_crtcs);
		goto out;
	}
	connector.connector_id = connectors[0];
	connector.modes_ptr = USER_PTR(modes);
	connector.count_modes = ARRAY_SIZE(modes);
	connector.encoders_ptr = USER_PTR(encoders);
	connector.count_encoders = ARRAY_SIZE(encoders);
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector)) {
		perror("get connector");
		goto out;
	}
	/* Connection value 1 is DRM's connected UAPI enum value. */
	if (connector.connection != 1 || !connector.count_modes ||
	    connector.count_modes > ARRAY_SIZE(modes) || !connector.count_encoders ||
	    connector.count_encoders > ARRAY_SIZE(encoders)) {
		fprintf(stderr, "No connected display with a supported mode list.\n");
		goto out;
	}
	mode = modes[0];
	for (i = 0; (unsigned int)i < connector.count_modes; i++)
		if (modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			mode = modes[i];
			break;
		}
	printf("%s: connector %u, CRTC %u, mode %.32s (%ux%u)\n",
	       device, connectors[0], crtcs[0], mode.name, mode.hdisplay, mode.vdisplay);
	if (info) {
		result = 0;
		goto out;
	}
	if (mode.hdisplay < 40 || mode.vdisplay < 40 ||
	    mode.hdisplay > 4096 || mode.vdisplay > 4096) {
		fprintf(stderr, "Unsupported display size.\n");
		goto out;
	}
	encoder.encoder_id = connector.encoder_id ? connector.encoder_id : encoders[0];
	if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) || !(encoder.possible_crtcs & 1)) {
		fprintf(stderr, "Connector encoder cannot use the display CRTC.\n");
		goto out;
	}
	if (display_signals() || ioctl(fd, DRM_IOCTL_SET_MASTER)) {
		perror("acquire DRM master");
		goto out;
	}
	master = 1;
	if (display_vt_graphics(&vt))
		goto out;
	saved.crtc_id = crtcs[0];
	if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &saved)) {
		perror("save CRTC");
		goto out;
	}
	dumb.width = mode.hdisplay;
	dumb.height = mode.vdisplay;
	dumb.bpp = bytes * 8;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb)) {
		perror("create dumb buffer");
		goto out;
	}
	if (dumb.pitch < dumb.width * bytes || dumb.size < (uint64_t)dumb.pitch * dumb.height ||
	    dumb.size > SIZE_MAX) {
		fprintf(stderr, "Invalid dumb-buffer size or pitch.\n");
		goto out;
	}
	mapping.handle = dumb.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mapping) || mapping.offset > INT64_MAX) {
		perror("map dumb buffer");
		goto out;
	}
	map = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mapping.offset);
	if (map == MAP_FAILED) {
		perror("mmap dumb buffer");
		goto out;
	}
	for (y = 0; y < dumb.height && !display_stop; y++)
		for (x = 0; x < dumb.width; x++)
			write_pixel(map, dumb.pitch, x, y, bytes,
				    display_rgb(x, y, dumb.width, dumb.height));
	fb.width = dumb.width;
	fb.height = dumb.height;
	fb.pixel_format = bytes == 2 ? DRM_FORMAT_RGB565 : DRM_FORMAT_XRGB8888;
	fb.handles[0] = dumb.handle;
	fb.pitches[0] = dumb.pitch;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb)) {
		perror("add framebuffer");
		goto out;
	}
	if (display_stop)
		goto out;
	set.crtc_id = crtcs[0];
	set.fb_id = fb.fb_id;
	set.set_connectors_ptr = USER_PTR(connectors);
	set.count_connectors = 1;
	set.mode = mode;
	set.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set)) {
		perror("set CRTC");
		goto out;
	}
	changed = 1;
	/* Odd x/y, width and height exercise partial copies and the last pixel. */
	{
		struct drm_clip_rect clip = { .x1 = 7, .y1 = 11, .x2 = 38, .y2 = 30 };
		struct drm_mode_fb_dirty_cmd dirty = {
			.fb_id = fb.fb_id, .num_clips = 1, .clips_ptr = USER_PTR(&clip)
		};
		for (y = clip.y1; y < clip.y2; y++)
			for (x = clip.x1; x < clip.x2; x++)
				write_pixel(map, dumb.pitch, x, y, bytes, 0xff8000);
		if (ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty)) {
			perror("submit damage rectangle");
			goto out;
		}
	}
	if (frames)
		printf("Benchmarking %u updates per rectangle (%s, pitch %u).\n",
		       frames, bytes == 2 ? "RGB565" : "XRGB8888", dumb.pitch);
	else
		printf("Pattern displayed for %u seconds (%s, pitch %u); orange damage rectangle at 7,11 size 31x19.\n",
		       seconds, bytes == 2 ? "RGB565" : "XRGB8888", dumb.pitch);
	fflush(stdout);
	if (frames) {
		if (benchmark(fd, fb.fb_id, map, &dumb, bytes, frames))
			goto out;
	} else {
		display_wait(seconds);
	}
	result = display_stop ? 128 + display_stop : 0;
out:
	if (changed) {
		saved.set_connectors_ptr = saved.mode_valid ? USER_PTR(connectors) : 0;
		saved.count_connectors = saved.mode_valid ? 1 : 0;
		if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &saved)) {
			perror("restore CRTC");
			result = 1;
		}
	}
	if (fb.fb_id)
		ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb.fb_id);
	if (map != MAP_FAILED)
		munmap(map, dumb.size);
	if (dumb.handle) {
		struct drm_mode_destroy_dumb destroy = { .handle = dumb.handle };
		ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
	if (master)
		ioctl(fd, DRM_IOCTL_DROP_MASTER);
	if (fd >= 0)
		close(fd);
	/* Lastclose lets the DRM fbdev client restore its mode before text resumes. */
	display_vt_restore(&vt);
	return result;
usage:
	fprintf(stderr, "usage: drmtest [--info] [--rgb565] [--seconds 1..300] [--benchmark 1..300] [--device /dev/dri/card0]\n");
	return 2;
}
