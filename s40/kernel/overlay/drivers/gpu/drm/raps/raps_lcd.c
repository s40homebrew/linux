// SPDX-License-Identifier: GPL-2.0-only
#include <linux/dma-mapping.h>
#include <linux/dma/raps.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include <drm/drm_rect.h>

#include "raps_lcd.h"
#include "raps_pixels.h"

#define RAPS_LCD_MODE 0x04
#define RAPS_LCD_STATUS 0x08
#define RAPS_LCD_DMA_MODE 0x0c
#define RAPS_LCD_FIFO 0x10
#define RAPS_DMA_MIN_BYTES 64

int raps_lcd_ready(struct raps_lcd_io *lcd)
{
	u32 status;
	int ret;

	if (lcd->failed || lcd->dma_stuck)
		return -EIO;
	ret = readl_poll_timeout_atomic(lcd->base + RAPS_LCD_STATUS, status,
					status & 8, 1, 1000);
	if (ret)
		lcd->failed = true;
	return ret;
}

int raps_lcd_transfer(struct raps_lcd_io *lcd, u32 value,
		      unsigned int count, bool data)
{
	int ret = raps_lcd_ready(lcd);

	if (ret)
		return ret;
	writel(2, lcd->base + RAPS_LCD_STATUS);
	writel(1, lcd->base + RAPS_LCD_STATUS);
	writel(lcd->bus_mode | (data ? 0x40000 : 0) | (count - 1),
	       lcd->base + RAPS_LCD_MODE);
	if (data && lcd->pio_delay_us)
		udelay(lcd->pio_delay_us);
	writel(value, lcd->base + RAPS_LCD_FIFO);
	return 0;
}

int raps_lcd_command(struct raps_lcd_io *lcd, u8 command)
{
	return raps_lcd_transfer(lcd, command, 1, false);
}

int raps_lcd_set_format(struct raps_lcd_io *lcd, u8 format)
{
	int ret;

	if (format != 0x55 && format != 0x06 && format != 0x07)
		return -EINVAL;
	ret = raps_lcd_command(lcd, 0x3a);
	if (!ret)
		ret = raps_lcd_transfer(lcd, format, 1, true);
	if (!ret)
		ret = raps_lcd_ready(lcd);
	if (!ret) {
		lcd->pixel_format = format;
		lcd->pixel_bytes = format == 0x55 ? 2 : 3;
	}
	return ret;
}

static u32 raps_lcd_window(unsigned int first, unsigned int last)
{
	return (first >> 8) | ((first & 255) << 8) |
	       ((last >> 8) << 16) | ((last & 255) << 24);
}

static int raps_lcd_set_window(struct raps_lcd_io *lcd, const struct drm_rect *rect)
{
	int ret;

	ret = raps_lcd_command(lcd, 0x2a);
	if (!ret)
		ret = raps_lcd_transfer(lcd, raps_lcd_window(rect->x1, rect->x2 - 1),
					4, true);
	if (!ret)
		ret = raps_lcd_command(lcd, 0x2b);
	if (!ret)
		ret = raps_lcd_transfer(lcd, raps_lcd_window(rect->y1, rect->y2 - 1),
					4, true);
	if (!ret)
		ret = raps_lcd_command(lcd, 0x2c);
	return ret;
}

static int raps_lcd_pio(struct raps_lcd_io *lcd, const u8 *pixels, unsigned int bytes)
{
	unsigned int offset;
	int ret;

	for (offset = 0; offset < bytes; offset += 4) {
		unsigned int count = min(bytes - offset, 4U);
		u32 word = raps_pixel_word(pixels + offset, count);

		ret = raps_lcd_transfer(lcd, word, count, true);
		if (ret)
			return ret;
		if (!(offset & 1023))
			cond_resched();
	}
	return raps_lcd_ready(lcd);
}

static void raps_lcd_dma_done(void *data)
{
	complete(data);
}

static void raps_lcd_unmap(struct raps_lcd_io *lcd)
{
	if (!lcd->mapped_bytes)
		return;
	dma_unmap_single(dmaengine_get_dma_device(lcd->dma), lcd->mapped_address,
			 lcd->mapped_bytes, DMA_TO_DEVICE);
	lcd->mapped_bytes = 0;
}

static void raps_lcd_snapshot(struct raps_lcd_io *lcd, u32 *snapshot)
{
	snapshot[0] = readl(lcd->base + RAPS_LCD_MODE);
	snapshot[1] = readl(lcd->base + RAPS_LCD_STATUS);
	snapshot[2] = readl(lcd->base + RAPS_LCD_DMA_MODE);
}

static int raps_lcd_dma(struct raps_lcd_io *lcd, unsigned int bytes,
			bool polling, bool diagnostic)
{
	struct device *dev = dmaengine_get_dma_device(lcd->dma);
	struct dma_async_tx_descriptor *desc;
	dma_addr_t address;
	dma_cookie_t cookie;
	unsigned int bulk = bytes & ~3U;
	int progress, ret;

	address = dma_map_single(dev, lcd->pixels, bulk, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, address))
		return -ENOMEM;
	lcd->mapped_address = address;
	lcd->mapped_bytes = bulk;
	desc = dmaengine_prep_slave_single(lcd->dma, address, bulk, DMA_MEM_TO_DEV,
					   DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc) {
		ret = -ENOMEM;
		goto unmap;
	}
	reinit_completion(&lcd->done);
	desc->callback = raps_lcd_dma_done;
	desc->callback_param = &lcd->done;
	cookie = dmaengine_submit(desc);
	ret = dma_submit_error(cookie);
	if (ret)
		goto unmap;
	ret = raps_lcd_ready(lcd);
	if (ret)
		goto terminate;
	writel(2, lcd->base + RAPS_LCD_STATUS);
	writel(1, lcd->base + RAPS_LCD_STATUS);
	writel(0x67, lcd->base + RAPS_LCD_DMA_MODE);
	writel(lcd->bus_mode | 0x40000 | (bulk - 1), lcd->base + RAPS_LCD_MODE);
	if (diagnostic)
		raps_lcd_snapshot(lcd, lcd->test_lcd_before);
	dma_async_issue_pending(lcd->dma);
	if (polling) {
		ret = read_poll_timeout(raps_dma_test_poll_complete, progress,
				       progress != 0, 10, 100000, false, lcd->dma);
		if (!ret)
			ret = progress < 0 ? progress : raps_lcd_ready(lcd);
	} else {
		ret = wait_for_completion_timeout(&lcd->done, msecs_to_jiffies(100)) ?
			0 : -ETIMEDOUT;
	}
	if (diagnostic)
		raps_lcd_snapshot(lcd, lcd->test_lcd_after);
	if (ret || polling)
		goto terminate;
	ret = raps_lcd_ready(lcd);
	goto unmap;
terminate:
	if (dmaengine_terminate_sync(lcd->dma)) {
		/* Never reuse memory that a controller still owns. */
		lcd->failed = true;
		lcd->dma_stuck = true;
		dmaengine_synchronize(lcd->dma);
		return -EIO;
	}
unmap:
	raps_lcd_unmap(lcd);
	if (!ret && bulk < bytes)
		ret = raps_lcd_pio(lcd, lcd->pixels + bulk, bytes - bulk);
	return ret;
}

int raps_lcd_send(struct raps_lcd_io *lcd, const struct drm_rect *damage)
{
	unsigned int bytes = drm_rect_width(damage) * drm_rect_height(damage) * lcd->pixel_bytes;
	int ret = raps_lcd_set_window(lcd, damage);

	if (ret)
		return ret;
	if (lcd->dma && lcd->dma_enabled && !lcd->dma_disabled &&
	    bytes >= RAPS_DMA_MIN_BYTES) {
		ret = raps_lcd_dma(lcd, bytes, false, false);
		if (!ret) {
			lcd->dma_updates++;
			lcd->bytes += bytes;
			return 0;
		}
		lcd->dma_errors++;
		lcd->dma_disabled = true;
		if (lcd->dma_stuck)
			return ret;
		dev_warn(dmaengine_get_dma_device(lcd->dma),
			 "LCD DMA failed: %d; switching to PIO\n", ret);
		/* Cancel the unfinished FIFO budget before replaying the rectangle. */
		writel(2, lcd->base + RAPS_LCD_STATUS);
		writel(1, lcd->base + RAPS_LCD_STATUS);
		lcd->failed = false;
		ret = raps_lcd_set_window(lcd, damage);
		if (ret)
			return ret;
	}
	ret = raps_lcd_pio(lcd, lcd->pixels, bytes);
	if (!ret) {
		lcd->pio_updates++;
		lcd->bytes += bytes;
	}
	return ret;
}

static u16 raps_lcd_test_pixel(unsigned int x, unsigned int y,
			      unsigned int width, unsigned int height)
{
	static const u16 bars[] = {
		0xffff, 0xffe0, 0x07ff, 0x07e0,
		0xf81f, 0xf800, 0x001f, 0x0000,
	};
	u16 gray;

	if (!x || !y || x == width - 1 || y == height - 1)
		return 0xffff;
	if (y < height / 2)
		return bars[x * ARRAY_SIZE(bars) / width];
	if (y < height * 3 / 4) {
		gray = x * 255 / (width - 1);
		return ((gray >> 3) << 11) | ((gray >> 2) << 5) | (gray >> 3);
	}
	return ((x / 8) ^ (y / 8)) & 1 ? 0xffff : 0;
}

int raps_lcd_dma_test(struct raps_lcd_io *lcd, bool polling,
		     unsigned int width, unsigned int height)
{
	struct drm_rect rect;
	unsigned int bytes;
	unsigned int x, y;
	int ret, recovery;

	if (!width || width > RAPS_LCD_WIDTH ||
	    !height || height > RAPS_LCD_HEIGHT || width * height < 2)
		return -EINVAL;
	if (!lcd->dma)
		return -ENODEV;
	if (lcd->failed || lcd->dma_stuck)
		return -EIO;
	dmaengine_synchronize(lcd->dma);
	ret = raps_dma_test_begin(lcd->dma, polling);
	if (ret)
		return ret;
	lcd->test_count++;
	lcd->test_polling = polling;
	lcd->test_width = width;
	lcd->test_height = height;
	lcd->test_pixel_bytes = lcd->pixel_bytes;
	bytes = width * height * lcd->pixel_bytes;
	rect.x1 = RAPS_LCD_WIDTH - width;
	rect.y1 = RAPS_LCD_HEIGHT - height;
	rect.x2 = RAPS_LCD_WIDTH;
	rect.y2 = RAPS_LCD_HEIGHT;
	memset(lcd->test_lcd_before, 0, sizeof(lcd->test_lcd_before));
	memset(lcd->test_lcd_after, 0, sizeof(lcd->test_lcd_after));
	if (width == 16 && height == 16) {
		for (x = 0; x < 16 * 16; ++x)
			put_unaligned_be16(polling ? 0x07e0 : 0xf81f,
					   lcd->pixels + x * 2);
	} else {
		for (y = 0; y < height; ++y)
			for (x = 0; x < width; ++x)
				put_unaligned_be16(raps_lcd_test_pixel(x, y, width, height),
					lcd->pixels + (y * width + x) * 2);
	}
	if (lcd->pixel_bytes == 3)
		raps_pixels_expand(lcd->pixels, width * height);
	ret = raps_lcd_set_window(lcd, &rect);
	if (!ret)
		ret = raps_lcd_dma(lcd, bytes, polling, true);
	if (!ret) {
		lcd->dma_updates++;
		lcd->bytes += bytes;
	} else {
		lcd->dma_errors++;
	}
	if (!lcd->dma_stuck) {
		/* Leave normal drawing on PIO even when the test times out. */
		writel(2, lcd->base + RAPS_LCD_STATUS);
		writel(1, lcd->base + RAPS_LCD_STATUS);
		lcd->failed = false;
		recovery = raps_lcd_ready(lcd);
		if (!ret)
			ret = recovery;
	}
	dmaengine_synchronize(lcd->dma);
	raps_dma_test_end(lcd->dma);
	lcd->test_result = ret;
	return ret;
}

static void raps_lcd_release_io(void *data)
{
	struct raps_lcd_io *lcd = data;

	if (lcd->dma) {
		if (dmaengine_terminate_sync(lcd->dma)) {
			dmaengine_synchronize(lcd->dma);
			dev_err(dmaengine_get_dma_device(lcd->dma),
				"LCD DMA remains active; retaining its buffer and channel\n");
			return;
		}
		raps_lcd_unmap(lcd);
		dma_release_channel(lcd->dma);
		lcd->dma = NULL;
	}
	kfree(lcd->pixels);
	lcd->pixels = NULL;
}

int raps_lcd_io_init(struct platform_device *pdev, struct raps_lcd_io *lcd)
{
	struct dma_slave_config config = {
		.direction = DMA_MEM_TO_DEV,
		.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.dst_maxburst = 1,
	};
	struct resource *resource;
	int ret;

	lcd->base = devm_platform_get_and_ioremap_resource(pdev, 0, &resource);
	if (IS_ERR(lcd->base))
		return PTR_ERR(lcd->base);
	lcd->pixels = kmalloc(RAPS_LCD_BYTES, GFP_KERNEL);
	if (!lcd->pixels)
		return -ENOMEM;
	ret = devm_add_action_or_reset(&pdev->dev, raps_lcd_release_io, lcd);
	if (ret)
		return ret;
	init_completion(&lcd->done);
	lcd->pixel_format = 0x55;
	lcd->pixel_bytes = 2;
	lcd->bus_mode = readl(lcd->base + RAPS_LCD_MODE) & 0x3ff00000;
	if (!of_property_present(pdev->dev.of_node, "dmas"))
		return 0;
	lcd->dma = dma_request_chan(&pdev->dev, "tx");
	if (IS_ERR(lcd->dma)) {
		ret = PTR_ERR(lcd->dma);
		lcd->dma = NULL;
		if (ret == -EPROBE_DEFER)
			return ret;
		dev_warn(&pdev->dev, "DMA unavailable: %d; using PIO\n", ret);
		return 0;
	}
	config.dst_addr = resource->start + RAPS_LCD_FIFO;
	return dmaengine_slave_config(lcd->dma, &config);
}
