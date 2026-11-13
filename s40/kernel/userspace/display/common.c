/* SPDX-License-Identifier: MIT */
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t display_stop;

static void display_signal(int sig)
{
	display_stop = sig;
}

int display_signals(void)
{
	struct sigaction action = { .sa_handler = display_signal };
	const int signals[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };
	unsigned int i;
	sigemptyset(&action.sa_mask);
	for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
		if (sigaction(signals[i], &action, NULL))
			return -1;
	return 0;
}

void display_vt_restore(struct display_vt *vt)
{
	if (vt->changed && ioctl(vt->active, KDSETMODE, vt->mode))
		perror("restore console mode");
	if (vt->locked && ioctl(vt->control, VT_UNLOCKSWITCH))
		perror("unlock console switching");
	if (vt->active >= 0)
		close(vt->active);
	if (vt->control >= 0)
		close(vt->control);
	vt->control = vt->active = -1;
	vt->changed = vt->locked = 0;
}

/* /dev/console can be USB ACM. Find and protect the actual visible VT. */
int display_vt_graphics(struct display_vt *vt)
{
	struct vt_stat state;
	char path[32];
	vt->control = vt->active = -1;
	vt->locked = vt->changed = 0;
	vt->control = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (vt->control < 0 || ioctl(vt->control, VT_LOCKSWITCH))
		goto fail;
	vt->locked = 1;
	if (ioctl(vt->control, VT_GETSTATE, &state))
		goto fail;
	snprintf(path, sizeof(path), "/dev/tty%u", state.v_active);
	vt->active = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (vt->active < 0 || ioctl(vt->active, KDGETMODE, &vt->mode))
		goto fail;
	if (vt->mode != KD_TEXT) {
		fprintf(stderr, "The active console is already in graphics mode.\n");
		errno = EBUSY;
		goto fail;
	}
	if (ioctl(vt->active, KDSETMODE, KD_GRAPHICS))
		goto fail;
	vt->changed = 1;
	return 0;
fail:
	perror("prepare display console");
	display_vt_restore(vt);
	return -1;
}

int display_seconds(const char *value, unsigned int *seconds)
{
	char *end;
	unsigned long parsed;
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno || !*value || *end || parsed < 1 || parsed > 300)
		return -1;
	*seconds = (unsigned int)parsed;
	return 0;
}

void display_wait(unsigned int seconds)
{
	struct timespec remaining = { .tv_sec = seconds };
	while (!display_stop && nanosleep(&remaining, &remaining) && errno == EINTR)
		;
}

/* RGB color bars, grayscale ramp, and an eight-pixel checkerboard. */
uint32_t display_rgb(unsigned int x, unsigned int y,
			    unsigned int width, unsigned int height)
{
	static const uint32_t bars[8] = {
		0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
		0xff00ff, 0xff0000, 0x0000ff, 0x000000
	};
	if (!x || !y || x + 1 == width || y + 1 == height)
		return 0xffffff;
	if (y < height / 2)
		return bars[(uint64_t)x * 8 / width];
	if (y < height * 3 / 4) {
		uint32_t gray = (uint64_t)x * 255 / (width - 1);
		return gray * 0x010101;
	}
	return ((x / 8) ^ (y / 8)) & 1 ? 0xffffff : 0;
}
