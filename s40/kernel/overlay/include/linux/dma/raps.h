/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_DMA_RAPS_H
#define _LINUX_DMA_RAPS_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct dma_chan;

#if IS_REACHABLE(CONFIG_RAPS_DMA)
int raps_dma_test_begin(struct dma_chan *chan, bool polling);
/* 1: hardware complete, 0: busy, negative: invalid or faulted diagnostic. */
int raps_dma_test_poll_complete(struct dma_chan *chan);
/* Terminate and synchronize the channel before ending a diagnostic. */
void raps_dma_test_end(struct dma_chan *chan);
ssize_t raps_dma_test_status(struct dma_chan *chan, char *buf, size_t size);
#else
static inline int raps_dma_test_begin(struct dma_chan *chan, bool polling)
{
	return -ENODEV;
}

static inline int raps_dma_test_poll_complete(struct dma_chan *chan)
{
	return -ENODEV;
}

static inline void raps_dma_test_end(struct dma_chan *chan)
{
}

static inline ssize_t raps_dma_test_status(struct dma_chan *chan, char *buf,
					  size_t size)
{
	if (buf && size)
		buf[0] = '\0';
	return 0;
}
#endif

#endif
