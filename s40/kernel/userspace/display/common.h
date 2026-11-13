/* SPDX-License-Identifier: MIT */
#ifndef DISPLAY_COMMON_H
#define DISPLAY_COMMON_H

#include <signal.h>
#include <stdint.h>

struct display_vt {
	int control, active, mode, locked, changed;
};

extern volatile sig_atomic_t display_stop;
int display_signals(void);
void display_vt_restore(struct display_vt *vt);
int display_vt_graphics(struct display_vt *vt);
int display_seconds(const char *value, unsigned int *seconds);
void display_wait(unsigned int seconds);
uint32_t display_rgb(unsigned int x, unsigned int y,
		     unsigned int width, unsigned int height);

#endif
