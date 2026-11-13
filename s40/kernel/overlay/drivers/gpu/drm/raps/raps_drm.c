// SPDX-License-Identifier: GPL-2.0-only
/* RAPS command-mode display: DRM/KMS, format conversion and console. */
#include <linux/dma-mapping.h>
#include <linux/dma/raps.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/nokia/rm443.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include "raps_lcd.h"
#include "raps_pixels.h"

struct raps_lcd {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct raps_lcd_io io;
	struct mutex lock;
	u64 update_us;
	u32 last_update_us;
};

static inline struct raps_lcd *to_raps_lcd(struct drm_device *drm)
{
	return container_of(drm, struct raps_lcd, drm);
}

/* Command mode has no programmable scanout clock. This DRM mode describes
 * the fixed image dimensions; it does not advertise a PIO frame rate.
 * The physical panel size has not been measured.
 */
static const struct drm_display_mode raps_lcd_mode = {
	DRM_SIMPLE_MODE(RAPS_LCD_WIDTH, RAPS_LCD_HEIGHT, 0, 0),
};

static int raps_lcd_flush(struct raps_lcd *lcd, struct drm_plane_state *state,
			  const struct drm_rect *damage)
{
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(state);
	struct iosys_map dst = IOSYS_MAP_INIT_VADDR(lcd->io.pixels);
	unsigned int pixels = drm_rect_width(damage) * drm_rect_height(damage);
	unsigned int pitch = drm_rect_width(damage) * lcd->io.pixel_bytes;
	ktime_t start = ktime_get();
	int ret;

	if (lcd->io.failed || lcd->io.dma_stuck)
		return -EIO;
	ret = drm_gem_fb_begin_cpu_access(state->fb, DMA_FROM_DEVICE);
	if (ret)
		return ret;
	if (state->fb->format->format == DRM_FORMAT_RGB565) {
		unsigned int packed_pitch = drm_rect_width(damage) * 2;

		drm_fb_swab(&dst, &packed_pitch, shadow->data, state->fb, damage,
			    false, &shadow->fmtcnv_state);
		if (lcd->io.pixel_bytes == 3)
			raps_pixels_expand(lcd->io.pixels, pixels);
	} else if (lcd->io.pixel_bytes == 3) {
		drm_fb_xrgb8888_to_rgb888(&dst, &pitch, shadow->data, state->fb,
					damage, &shadow->fmtcnv_state);
		raps_pixels_rgb_order(lcd->io.pixels, pixels);
	} else {
		drm_fb_xrgb8888_to_rgb565(&dst, &pitch, shadow->data, state->fb,
					damage, &shadow->fmtcnv_state, true);
	}
	drm_gem_fb_end_cpu_access(state->fb, DMA_FROM_DEVICE);
	ret = raps_lcd_send(&lcd->io, damage);
	lcd->last_update_us = ktime_us_delta(ktime_get(), start);
	lcd->update_us += lcd->last_update_us;
	return ret;
}

static enum drm_mode_status raps_lcd_mode_valid(struct drm_simple_display_pipe *pipe,
					       const struct drm_display_mode *mode)
{
	return drm_crtc_helper_mode_valid_fixed(&pipe->crtc, mode, &raps_lcd_mode);
}

static int raps_lcd_check(struct drm_simple_display_pipe *pipe,
			  struct drm_plane_state *plane_state,
			  struct drm_crtc_state *crtc_state)
{
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(plane_state);

	/* Completion events describe finished transfers, not panel vblank. */
	crtc_state->no_vblank = true;
	/* Format helpers cannot report a scratch-allocation failure. Reserve
	 * the largest input row here, while the atomic commit can still fail.
	 */
	if (!drm_format_conv_state_reserve(&shadow->fmtcnv_state,
					   RAPS_LCD_WIDTH * 4, GFP_KERNEL))
		return -ENOMEM;
	return 0;
}

static void raps_lcd_enable(struct drm_simple_display_pipe *pipe,
			    struct drm_crtc_state *crtc_state,
			    struct drm_plane_state *plane_state)
{
	struct raps_lcd *lcd = to_raps_lcd(pipe->crtc.dev);
	struct drm_rect rect = { 0, 0, RAPS_LCD_WIDTH, RAPS_LCD_HEIGHT };
	int idx, ret;

	if (!drm_dev_enter(&lcd->drm, &idx))
		return;
	mutex_lock(&lcd->lock);
	/* A deliberate disable/enable permits retry after a bus timeout. */
	lcd->io.failed = false;
	ret = raps_lcd_set_format(&lcd->io, lcd->io.pixel_format);
	if (!ret)
		ret = raps_lcd_command(&lcd->io, 0x29);
	if (!ret)
		ret = raps_lcd_flush(lcd, plane_state, &rect);
	mutex_unlock(&lcd->lock);
	if (ret)
		drm_err_ratelimited(&lcd->drm, "LCD enable failed: %d\n", ret);
	drm_dev_exit(idx);
}

static void raps_lcd_disable(struct drm_simple_display_pipe *pipe)
{
	struct raps_lcd *lcd = to_raps_lcd(pipe->crtc.dev);
	int idx;

	if (!drm_dev_enter(&lcd->drm, &idx))
		return;
	mutex_lock(&lcd->lock);
	/* Display off, without changing the inherited power/clock setup. */
	raps_lcd_command(&lcd->io, 0x28);
	mutex_unlock(&lcd->lock);
	drm_dev_exit(idx);
}

static void raps_lcd_update(struct drm_simple_display_pipe *pipe,
			    struct drm_plane_state *old_state)
{
	struct raps_lcd *lcd = to_raps_lcd(pipe->crtc.dev);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_rect damage;
	int idx, ret;

	if (!pipe->crtc.state->active || !state->fb)
		return;
	if (!drm_atomic_helper_damage_merged(old_state, state, &damage))
		return;
	if (!drm_dev_enter(&lcd->drm, &idx))
		return;
	mutex_lock(&lcd->lock);
	ret = raps_lcd_flush(lcd, state, &damage);
	mutex_unlock(&lcd->lock);
	if (ret)
		drm_err_ratelimited(&lcd->drm, "LCD update failed: %d\n", ret);
	drm_dev_exit(idx);
}

static const struct drm_simple_display_pipe_funcs raps_lcd_pipe_funcs = {
	.mode_valid = raps_lcd_mode_valid,
	.check = raps_lcd_check,
	.enable = raps_lcd_enable,
	.disable = raps_lcd_disable,
	.update = raps_lcd_update,
	DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS,
};

static int raps_lcd_get_modes(struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &raps_lcd_mode);
}

static const struct drm_connector_helper_funcs raps_lcd_connector_helper_funcs = {
	.get_modes = raps_lcd_get_modes,
};

static const struct drm_connector_funcs raps_lcd_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs raps_lcd_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_DMA_FOPS(raps_lcd_fops);

static const struct drm_driver raps_lcd_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &raps_lcd_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	.name = "raps-lcd",
	.desc = "RAPS LCD controller",
	.date = "20261112",
	.major = 1,
	.minor = 0,
};

static const u32 raps_lcd_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const u64 raps_lcd_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static const char *raps_lcd_transport(struct raps_lcd_io *io)
{
	return io->dma && io->dma_enabled && !io->dma_disabled ? "dma" : "pio";
}

static const char *raps_lcd_format(struct raps_lcd_io *io)
{
	return io->pixel_format == 0x07 ? "rgb888" :
	       io->pixel_format == 0x06 ? "rgb666" : "rgb565";
}

static ssize_t transfer_stats_show(struct device *dev,
				   struct device_attribute *attr, char *buffer)
{
	struct raps_lcd *lcd = to_raps_lcd(dev_get_drvdata(dev));
	ssize_t len;

	mutex_lock(&lcd->lock);
	len = sysfs_emit(buffer,
		"transport=%s format=%s dma_updates=%llu pio_updates=%llu dma_errors=%u\n"
		"bytes=%llu total_us=%llu last_us=%u\n",
		raps_lcd_transport(&lcd->io), raps_lcd_format(&lcd->io),
		lcd->io.dma_updates, lcd->io.pio_updates, lcd->io.dma_errors,
		lcd->io.bytes, lcd->update_us, lcd->last_update_us);
	mutex_unlock(&lcd->lock);
	return len;
}
static DEVICE_ATTR_RO(transfer_stats);

static ssize_t transport_show(struct device *dev,
			      struct device_attribute *attr, char *buffer)
{
	struct raps_lcd *lcd = to_raps_lcd(dev_get_drvdata(dev));
	ssize_t len;

	mutex_lock(&lcd->lock);
	len = sysfs_emit(buffer, "%s\n", raps_lcd_transport(&lcd->io));
	mutex_unlock(&lcd->lock);
	return len;
}

static ssize_t transport_store(struct device *dev, struct device_attribute *attr,
			       const char *buffer, size_t count)
{
	struct raps_lcd *lcd = to_raps_lcd(dev_get_drvdata(dev));
	bool dma;
	int idx, ret = 0;

	if (sysfs_streq(buffer, "dma"))
		dma = true;
	else if (sysfs_streq(buffer, "pio"))
		dma = false;
	else
		return -EINVAL;
	if (!drm_dev_enter(&lcd->drm, &idx))
		return -ENODEV;
	mutex_lock(&lcd->lock);
	if (lcd->io.dma_stuck || lcd->io.failed)
		ret = -EIO;
	else if (dma && !lcd->io.dma)
		ret = -ENODEV;
	else {
		lcd->io.dma_enabled = dma;
		if (dma)
			lcd->io.dma_disabled = false;
	}
	mutex_unlock(&lcd->lock);
	drm_dev_exit(idx);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(transport);

static ssize_t pixel_format_show(struct device *dev,
				 struct device_attribute *attr, char *buffer)
{
	struct raps_lcd *lcd = to_raps_lcd(dev_get_drvdata(dev));
	ssize_t len;

	mutex_lock(&lcd->lock);
	len = sysfs_emit(buffer, "%s\n", raps_lcd_format(&lcd->io));
	mutex_unlock(&lcd->lock);
	return len;
}

static ssize_t pixel_format_store(struct device *dev, struct device_attribute *attr,
				  const char *buffer, size_t count)
{
	struct raps_lcd *lcd = to_raps_lcd(dev_get_drvdata(dev));
	u8 format;
	int idx, ret;

	if (sysfs_streq(buffer, "rgb565"))
		format = 0x55;
	else if (sysfs_streq(buffer, "rgb666"))
		format = 0x06;
	else if (sysfs_streq(buffer, "rgb888"))
		format = 0x07;
	else
		return -EINVAL;
	if (!drm_dev_enter(&lcd->drm, &idx))
		return -ENODEV;
	mutex_lock(&lcd->lock);
	ret = raps_lcd_set_format(&lcd->io, format);
	mutex_unlock(&lcd->lock);
	drm_dev_exit(idx);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(pixel_format);

static ssize_t pio_delay_us_show(struct device *dev,
			       struct device_attribute *attr, char *buffer)
{
	struct raps_lcd *lcd = to_raps_lcd(dev_get_drvdata(dev));

	return sysfs_emit(buffer, "%u\n", READ_ONCE(lcd->io.pio_delay_us));
}

static ssize_t pio_delay_us_store(struct device *dev, struct device_attribute *attr,
				const char *buffer, size_t count)
{
	struct raps_lcd *lcd = to_raps_lcd(dev_get_drvdata(dev));
	unsigned int delay;
	int ret = kstrtouint(buffer, 0, &delay);

	if (ret)
		return ret;
	if (delay > 10)
		return -ERANGE;
	mutex_lock(&lcd->lock);
	WRITE_ONCE(lcd->io.pio_delay_us, delay);
	mutex_unlock(&lcd->lock);
	return count;
}
static DEVICE_ATTR_RW(pio_delay_us);

static ssize_t dma_test_show(struct device *dev,
			     struct device_attribute *attr, char *buffer)
{
	struct raps_lcd *lcd = to_raps_lcd(dev_get_drvdata(dev));
	struct raps_lcd_io *io = &lcd->io;
	ssize_t len;

	mutex_lock(&lcd->lock);
	len = sysfs_emit(buffer,
		"available=%u tests=%u mode=%s width=%u height=%u pixel_bytes=%u result=%d failed=%u stuck=%u\n"
		"lcd_before mode=%08x status=%08x dma=%08x\n"
		"lcd_after mode=%08x status=%08x dma=%08x\n",
		!!io->dma, io->test_count, io->test_count ?
		(io->test_polling ? "poll" : "irq") : "none",
		io->test_width, io->test_height, io->test_pixel_bytes,
		io->test_result, io->failed, io->dma_stuck,
		io->test_lcd_before[0], io->test_lcd_before[1], io->test_lcd_before[2],
		io->test_lcd_after[0], io->test_lcd_after[1], io->test_lcd_after[2]);
	if (io->dma)
		len += raps_dma_test_status(io->dma, buffer + len, PAGE_SIZE - len);
	mutex_unlock(&lcd->lock);
	return len;
}

static ssize_t dma_test_store(struct device *dev, struct device_attribute *attr,
			      const char *buffer, size_t count)
{
	struct raps_lcd *lcd = to_raps_lcd(dev_get_drvdata(dev));
	char mode[16], width_arg[16], height_arg[16], extra;
	unsigned int width = 16, height = 16;
	bool polling;
	int idx, ret, args;

	args = sscanf(buffer, "%15s %15s %15s %c", mode, width_arg, height_arg, &extra);
	if (args != 1 && args != 3)
		return -EINVAL;
	if (!strcmp(mode, "poll"))
		polling = true;
	else if (!strcmp(mode, "irq"))
		polling = false;
	else if (args == 1 && !strcmp(mode, "poll-full")) {
		polling = true;
		width = RAPS_LCD_WIDTH;
		height = RAPS_LCD_HEIGHT;
	} else if (args == 1 && !strcmp(mode, "irq-full")) {
		polling = false;
		width = RAPS_LCD_WIDTH;
		height = RAPS_LCD_HEIGHT;
	} else
		return -EINVAL;
	if (args == 3 && (kstrtouint(width_arg, 10, &width) ||
			 kstrtouint(height_arg, 10, &height)))
		return -EINVAL;
	if (!width || width > RAPS_LCD_WIDTH || !height ||
	    height > RAPS_LCD_HEIGHT || width * height < 2)
		return -EINVAL;
	if (!drm_dev_enter(&lcd->drm, &idx))
		return -ENODEV;
	dev_notice(dev, "DMA test: %s, %ux%u pixels\n",
		   polling ? "poll" : "irq", width, height);
	mutex_lock(&lcd->lock);
	ret = raps_lcd_dma_test(&lcd->io, polling, width, height);
	mutex_unlock(&lcd->lock);
	dev_notice(dev, "DMA test: %s result=%d\n",
		   polling ? "poll" : "irq", ret);
	drm_dev_exit(idx);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(dma_test);

static struct attribute *raps_lcd_attrs[] = {
	&dev_attr_transfer_stats.attr,
	&dev_attr_dma_test.attr,
	&dev_attr_pio_delay_us.attr,
	&dev_attr_transport.attr,
	&dev_attr_pixel_format.attr,
	NULL,
};

static const struct attribute_group raps_lcd_group = {
	.attrs = raps_lcd_attrs,
};

static int raps_lcd_probe(struct platform_device *pdev)
{
	struct raps_lcd *lcd;
	struct drm_device *drm;
	int ret;

	rm443_boot_stage(0x57);
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "LCD framebuffer DMA mask unavailable\n");
	lcd = devm_drm_dev_alloc(&pdev->dev, &raps_lcd_drm_driver,
				 struct raps_lcd, drm);
	if (IS_ERR(lcd))
		return PTR_ERR(lcd);
	drm = &lcd->drm;
	ret = raps_lcd_io_init(pdev, &lcd->io);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "LCD transport initialization failed\n");
	rm443_boot_stage(0x58);
	mutex_init(&lcd->lock);

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = RAPS_LCD_WIDTH;
	drm->mode_config.max_width = RAPS_LCD_WIDTH;
	drm->mode_config.min_height = RAPS_LCD_HEIGHT;
	drm->mode_config.max_height = RAPS_LCD_HEIGHT;
	drm->mode_config.funcs = &raps_lcd_mode_config_funcs;

	/* DRM has no parallel DBI connector type. This is an internal panel,
	 * with no EDID, hotplug detection, or external connector protocol.
	 */
	ret = drm_connector_init(drm, &lcd->connector, &raps_lcd_connector_funcs,
				 DRM_MODE_CONNECTOR_Unknown);
	if (ret)
		return ret;
	drm_connector_helper_add(&lcd->connector, &raps_lcd_connector_helper_funcs);
	ret = drm_simple_display_pipe_init(drm, &lcd->pipe, &raps_lcd_pipe_funcs,
					  raps_lcd_formats, ARRAY_SIZE(raps_lcd_formats),
					  raps_lcd_modifiers, &lcd->connector);
	if (ret)
		return ret;
	drm_plane_enable_fb_damage_clips(&lcd->pipe.plane);
	drm_mode_config_reset(drm);

	/* Stop direct boot-marker writes before touching the shared FIFO. */
	rm443_boot_stage(0x59);
	rm443_lcd_takeover();
	ret = raps_lcd_command(&lcd->io, 0x34); /* Disable tearing-effect output. */
	if (!ret)
		ret = raps_lcd_set_format(&lcd->io, 0x55);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "LCD controller not ready\n");
	platform_set_drvdata(pdev, drm);
	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;
	drm_fbdev_dma_setup(drm, 16);
	if (drm->fb_helper)
		drm->fb_helper->fbdefio.delay = max_t(unsigned long, 1, msecs_to_jiffies(10));
	ret = devm_device_add_group(&pdev->dev, &raps_lcd_group);
	if (ret) {
		drm_dev_unplug(drm);
		drm_atomic_helper_shutdown(drm);
		return ret;
	}
	dev_info(&pdev->dev, "RAPS LCD, 240x320 RGB565, PIO updates; DMA tests %s\n",
		 lcd->io.dma ? "available" : "unavailable");
	return 0;
}

static void raps_lcd_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static void raps_lcd_shutdown(struct platform_device *pdev)
{
	drm_atomic_helper_shutdown(platform_get_drvdata(pdev));
}

static const struct of_device_id raps_lcd_match[] = {
	{ .compatible = "nokia,rm443-lcd" },
	{}
};
MODULE_DEVICE_TABLE(of, raps_lcd_match);

static struct platform_driver raps_lcd_driver = {
	.probe = raps_lcd_probe,
	.remove_new = raps_lcd_remove,
	.shutdown = raps_lcd_shutdown,
	.driver = { .name = "raps-lcd", .of_match_table = raps_lcd_match,
		    .suppress_bind_attrs = true },
};
module_platform_driver(raps_lcd_driver);

MODULE_DESCRIPTION("RAPS LCD controller on Nokia RM-443");
MODULE_LICENSE("GPL");
