/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RAPS_LCD_H
#define RAPS_LCD_H

#include <linux/completion.h>
#include <linux/dmaengine.h>
#include <linux/types.h>

struct platform_device;
struct drm_rect;

#define RAPS_LCD_WIDTH 240
#define RAPS_LCD_HEIGHT 320
#define RAPS_LCD_BYTES (RAPS_LCD_WIDTH * RAPS_LCD_HEIGHT * 3)

struct raps_lcd_io {
	void __iomem *base;
	struct dma_chan *dma;
	struct completion done;
	u8 *pixels;
	dma_addr_t mapped_address;
	unsigned int mapped_bytes;
	u32 bus_mode;
	u32 pio_delay_us;
	u8 pixel_format;
	u8 pixel_bytes;
	bool failed;
	bool dma_enabled;
	bool dma_disabled;
	bool dma_stuck;
	u64 dma_updates;
	u64 pio_updates;
	u64 bytes;
	u32 dma_errors;
	u32 test_count;
	bool test_polling;
	u16 test_width;
	u16 test_height;
	u8 test_pixel_bytes;
	int test_result;
	u32 test_lcd_before[3];
	u32 test_lcd_after[3];
};

int raps_lcd_io_init(struct platform_device *pdev, struct raps_lcd_io *lcd);
int raps_lcd_command(struct raps_lcd_io *lcd, u8 command);
int raps_lcd_transfer(struct raps_lcd_io *lcd, u32 value, unsigned int count, bool data);
int raps_lcd_ready(struct raps_lcd_io *lcd);
int raps_lcd_set_format(struct raps_lcd_io *lcd, u8 format);
int raps_lcd_send(struct raps_lcd_io *lcd, const struct drm_rect *damage);
int raps_lcd_dma_test(struct raps_lcd_io *lcd, bool polling,
		     unsigned int width, unsigned int height);

#endif
