// SPDX-License-Identifier: GPL-2.0-only
#include <linux/dma-mapping.h>
#include <linux/dma/raps.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/nokia/rm443.h>

#include "virt-dma.h"

#define RAPS_CHANNELS 6
#define RAPS_STRIDE 0x400
#define RAPS_ENABLE 0x04
#define RAPS_COMMAND 0x08
#define RAPS_PRELOAD 0x0c
#define RAPS_PRELOAD_BITS 0x10
#define RAPS_SOURCE 0x14
#define RAPS_SOURCE_STEP 0x18
#define RAPS_SOURCE_BITS 0x1c
#define RAPS_DESTINATION 0x20
#define RAPS_DESTINATION_STEP 0x24
#define RAPS_DESTINATION_BITS 0x28
#define RAPS_COUNT 0x2c
#define RAPS_REQUEST_MODE 0x34
#define RAPS_WORD_SET 0x04
#define RAPS_WORD_CLEAR 0x08
#define RAPS_LCD_REQUEST 9

struct raps_dma_segment {
	dma_addr_t address;
	u32 bytes;
};

struct raps_dma_desc {
	struct virt_dma_desc vd;
	dma_addr_t destination;
	unsigned int count;
	struct raps_dma_segment segments[] __counted_by(count);
};

struct raps_dma_snapshot {
	u32 enable;
	u32 command;
	u32 source;
	u32 destination;
	u32 count;
	u32 mux;
	u32 control;
	u32 mux_global;
	bool irq_enabled;
};

struct raps_dma_test {
	bool active;
	bool polling;
	bool synchronized;
	u32 starts;
	u32 interrupts;
	u32 completions;
	u32 stops;
	u32 stop_errors;
	struct raps_dma_snapshot start;
	struct raps_dma_snapshot first_irq;
	struct raps_dma_snapshot pre_stop;
};

struct raps_dma_chan {
	struct virt_dma_chan vc;
	void __iomem *base;
	void __iomem *mux;
	void __iomem *control;
	void __iomem *mux_global;
	struct raps_dma_desc *active;
	struct dma_slave_config config;
	unsigned int index;
	unsigned int segment;
	int irq;
	bool irq_enabled;
	bool faulted;
	struct raps_dma_test test;
};

struct raps_dma {
	struct dma_device device;
	struct raps_dma_chan channels[RAPS_CHANNELS];
};

static struct raps_dma_chan *to_raps_chan(struct dma_chan *chan)
{
	return container_of(chan, struct raps_dma_chan, vc.chan);
}

static void raps_desc_free(struct virt_dma_desc *vd)
{
	kfree(container_of(vd, struct raps_dma_desc, vd));
}

static void raps_snapshot(struct raps_dma_chan *chan,
			  struct raps_dma_snapshot *snapshot)
{
	snapshot->enable = readl(chan->base + RAPS_ENABLE);
	snapshot->command = readl(chan->base + RAPS_COMMAND);
	snapshot->source = readl(chan->base + RAPS_SOURCE);
	snapshot->destination = readl(chan->base + RAPS_DESTINATION);
	snapshot->count = readl(chan->base + RAPS_COUNT);
	snapshot->mux = readl(chan->mux);
	snapshot->control = readl(chan->control);
	snapshot->mux_global = readl(chan->mux_global);
	snapshot->irq_enabled = chan->irq_enabled;
}

static void raps_mask_irq(struct raps_dma_chan *chan)
{
	if (chan->irq_enabled) {
		chan->irq_enabled = false;
		disable_irq_nosync(chan->irq);
	}
}

static int raps_stop(struct raps_dma_chan *chan)
{
	u32 command;
	int ret;

	if (chan->test.active) {
		raps_snapshot(chan, &chan->test.pre_stop);
		chan->test.stops++;
	}
	raps_mask_irq(chan);
	writel(0, chan->base + RAPS_COMMAND);
	ret = readl_poll_timeout_atomic(chan->base + RAPS_COMMAND, command,
					!command, 1, 1000);
	writel(0, chan->base + RAPS_ENABLE);
	readl(chan->base + RAPS_ENABLE);
	if (ret && chan->test.active)
		chan->test.stop_errors++;
	return ret;
}

static void raps_fault(struct raps_dma_chan *chan)
{
	if (!chan->faulted) {
		chan->faulted = true;
		raps_mask_irq(chan);
	}
}

static void raps_start_segment(struct raps_dma_chan *chan)
{
	struct raps_dma_desc *desc = chan->active;
	struct raps_dma_segment *segment = &desc->segments[chan->segment];

	writel(1, chan->base + RAPS_ENABLE);
	writel(0, chan->base + RAPS_PRELOAD);
	writel(0, chan->base + RAPS_PRELOAD_BITS);
	writel(1, chan->base + RAPS_REQUEST_MODE);
	writel(BIT(chan->index + 9), chan->control + RAPS_WORD_SET);
	writel(segment->address, chan->base + RAPS_SOURCE);
	writel(4, chan->base + RAPS_SOURCE_STEP);
	writel(32, chan->base + RAPS_SOURCE_BITS);
	writel(desc->destination, chan->base + RAPS_DESTINATION);
	writel(0, chan->base + RAPS_DESTINATION_STEP);
	writel(32, chan->base + RAPS_DESTINATION_BITS);
	writel(segment->bytes / 4 - 1, chan->base + RAPS_COUNT);
	writel(RAPS_LCD_REQUEST, chan->mux);
	writel(2, chan->base + RAPS_COMMAND);
	if (chan->test.active) {
		chan->test.starts++;
		chan->test.synchronized = false;
		raps_snapshot(chan, &chan->test.start);
	}
	if (!chan->test.active || !chan->test.polling) {
		chan->irq_enabled = true;
		enable_irq(chan->irq);
	}
}

static void raps_start_next(struct raps_dma_chan *chan)
{
	struct virt_dma_desc *vd = vchan_next_desc(&chan->vc);

	if (!vd)
		return;
	list_del(&vd->node);
	chan->active = container_of(vd, struct raps_dma_desc, vd);
	chan->segment = 0;
	raps_start_segment(chan);
}

static irqreturn_t raps_dma_interrupt(int irq, void *data)
{
	struct raps_dma_chan *chan = data;
	struct raps_dma_desc *desc;

	spin_lock(&chan->vc.lock);
	if (chan->test.active) {
		if (!chan->test.interrupts)
			raps_snapshot(chan, &chan->test.first_irq);
		chan->test.interrupts++;
	}
	if (chan->faulted)
		goto out;
	/* The channel command acknowledges the level, not the INTC vector. */
	if (raps_stop(chan)) {
		raps_fault(chan);
		goto out;
	}
	desc = chan->active;
	if (desc) {
		if (++chan->segment < desc->count) {
			raps_start_segment(chan);
		} else {
			chan->active = NULL;
			if (chan->test.active)
				chan->test.completions++;
			vchan_cookie_complete(&desc->vd);
			raps_start_next(chan);
		}
	}
out:
	spin_unlock(&chan->vc.lock);
	return IRQ_HANDLED;
}

static int raps_config(struct dma_chan *dma_chan, struct dma_slave_config *config)
{
	struct raps_dma_chan *chan = to_raps_chan(dma_chan);

	if (READ_ONCE(chan->faulted))
		return -EIO;
	if (config->direction != DMA_MEM_TO_DEV ||
	    config->dst_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES ||
	    config->dst_addr & 3 || upper_32_bits(config->dst_addr) ||
	    config->dst_maxburst > 1 || config->device_fc)
		return -EINVAL;
	chan->config = *config;
	return 0;
}

static struct dma_async_tx_descriptor *raps_prep_slave_sg(struct dma_chan *dma_chan,
		struct scatterlist *sgl, unsigned int sg_len,
		enum dma_transfer_direction direction, unsigned long flags, void *context)
{
	struct raps_dma_chan *chan = to_raps_chan(dma_chan);
	struct raps_dma_desc *desc;
	struct scatterlist *sg;
	unsigned int i;

	if (READ_ONCE(chan->faulted) || !sg_len || direction != DMA_MEM_TO_DEV ||
	    chan->config.dst_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES)
		return NULL;
	for_each_sg(sgl, sg, sg_len, i) {
		if (!sg_dma_len(sg) || (sg_dma_address(sg) | sg_dma_len(sg)) & 3 ||
		    upper_32_bits(sg_dma_address(sg)) ||
		    sg_dma_len(sg) - 1 > U32_MAX - sg_dma_address(sg))
			return NULL;
	}
	desc = kzalloc(struct_size(desc, segments, sg_len), GFP_NOWAIT);
	if (!desc)
		return NULL;
	desc->destination = chan->config.dst_addr;
	desc->count = sg_len;
	for_each_sg(sgl, sg, sg_len, i) {
		desc->segments[i].address = sg_dma_address(sg);
		desc->segments[i].bytes = sg_dma_len(sg);
	}
	return vchan_tx_prep(&chan->vc, &desc->vd, flags);
}

static void raps_issue_pending(struct dma_chan *dma_chan)
{
	struct raps_dma_chan *chan = to_raps_chan(dma_chan);
	unsigned long flags;

	spin_lock_irqsave(&chan->vc.lock, flags);
	if (!chan->faulted && vchan_issue_pending(&chan->vc) && !chan->active)
		raps_start_next(chan);
	spin_unlock_irqrestore(&chan->vc.lock, flags);
}

static int raps_terminate_all(struct dma_chan *dma_chan)
{
	struct raps_dma_chan *chan = to_raps_chan(dma_chan);
	unsigned long flags;
	int ret;
	LIST_HEAD(head);

	spin_lock_irqsave(&chan->vc.lock, flags);
	ret = raps_stop(chan);
	if (ret) {
		raps_fault(chan);
	} else {
		if (chan->active) {
			vchan_terminate_vdesc(&chan->active->vd);
			chan->active = NULL;
		}
		vchan_get_all_descriptors(&chan->vc, &head);
	}
	spin_unlock_irqrestore(&chan->vc.lock, flags);
	vchan_dma_desc_free_list(&chan->vc, &head);
	return ret;
}

static void raps_synchronize(struct dma_chan *dma_chan)
{
	struct raps_dma_chan *chan = to_raps_chan(dma_chan);
	unsigned long flags;

	synchronize_irq(chan->irq);
	vchan_synchronize(&chan->vc);
	spin_lock_irqsave(&chan->vc.lock, flags);
	if (chan->test.active && !chan->active)
		chan->test.synchronized = true;
	spin_unlock_irqrestore(&chan->vc.lock, flags);
}

static void raps_free_resources(struct dma_chan *dma_chan)
{
	struct raps_dma_chan *chan = to_raps_chan(dma_chan);
	int ret;

	ret = raps_terminate_all(dma_chan);
	raps_synchronize(dma_chan);
	if (ret)
		return;
	vchan_free_chan_resources(&chan->vc);
	memset(&chan->config, 0, sizeof(chan->config));
}

static int raps_alloc_resources(struct dma_chan *dma_chan)
{
	return READ_ONCE(to_raps_chan(dma_chan)->faulted) ? -EIO : 0;
}

static enum dma_status raps_tx_status(struct dma_chan *dma_chan,
				     dma_cookie_t cookie, struct dma_tx_state *state)
{
	struct raps_dma_chan *chan = to_raps_chan(dma_chan);
	enum dma_status status;
	unsigned long flags;

	spin_lock_irqsave(&chan->vc.lock, flags);
	status = dma_cookie_status(dma_chan, cookie, state);
	if (status != DMA_COMPLETE && chan->faulted)
		status = DMA_ERROR;
	spin_unlock_irqrestore(&chan->vc.lock, flags);
	return status;
}

static struct raps_dma_chan *raps_test_channel(struct dma_chan *dma_chan)
{
	if (IS_ERR_OR_NULL(dma_chan) || !dma_chan->device ||
	    dma_chan->device->device_issue_pending != raps_issue_pending)
		return NULL;
	return to_raps_chan(dma_chan);
}

static bool raps_has_descriptors(struct raps_dma_chan *chan)
{
	return chan->active || !list_empty(&chan->vc.desc_allocated) ||
		!list_empty(&chan->vc.desc_submitted) ||
		!list_empty(&chan->vc.desc_issued) ||
		!list_empty(&chan->vc.desc_completed) ||
		!list_empty(&chan->vc.desc_terminated);
}

int raps_dma_test_begin(struct dma_chan *dma_chan, bool polling)
{
	struct raps_dma_chan *chan = raps_test_channel(dma_chan);
	unsigned long flags;
	int ret = 0;

	if (!chan)
		return -EINVAL;
	spin_lock_irqsave(&chan->vc.lock, flags);
	if (chan->faulted) {
		ret = -EIO;
	} else if (chan->test.active || chan->irq_enabled ||
		   raps_has_descriptors(chan)) {
		ret = -EBUSY;
	} else {
		memset(&chan->test, 0, sizeof(chan->test));
		chan->test.active = true;
		chan->test.polling = polling;
	}
	spin_unlock_irqrestore(&chan->vc.lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(raps_dma_test_begin);

int raps_dma_test_poll_complete(struct dma_chan *dma_chan)
{
	struct raps_dma_chan *chan = raps_test_channel(dma_chan);
	struct raps_dma_segment *segment;
	unsigned long flags;
	u32 command, source, count, end;
	int ret;

	if (!chan)
		return -EINVAL;
	spin_lock_irqsave(&chan->vc.lock, flags);
	if (chan->faulted) {
		ret = -EIO;
		goto out;
	}
	if (!chan->test.active || !chan->test.polling || chan->irq_enabled ||
	    !chan->active || chan->active->count != 1 || chan->segment) {
		ret = -EINVAL;
		goto out;
	}
	segment = &chan->active->segments[0];
	end = lower_32_bits(segment->address + segment->bytes);
	command = readl(chan->base + RAPS_COMMAND);
	source = readl(chan->base + RAPS_SOURCE);
	count = readl(chan->base + RAPS_COUNT);
	ret = !command && source == end && (count & 0xffffff) == 0xffffff;
out:
	spin_unlock_irqrestore(&chan->vc.lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(raps_dma_test_poll_complete);

void raps_dma_test_end(struct dma_chan *dma_chan)
{
	struct raps_dma_chan *chan = raps_test_channel(dma_chan);
	unsigned long flags;

	if (!chan)
		return;
	spin_lock_irqsave(&chan->vc.lock, flags);
	if (chan->test.synchronized && !chan->irq_enabled &&
	    !raps_has_descriptors(chan))
		chan->test.active = false;
	spin_unlock_irqrestore(&chan->vc.lock, flags);
}
EXPORT_SYMBOL_GPL(raps_dma_test_end);

static ssize_t raps_snapshot_format(char *buf, size_t size, const char *name,
				    const struct raps_dma_snapshot *snapshot)
{
	return scnprintf(buf, size,
		"%s: enable=%x command=%x source=%08x destination=%08x count=%x "
		"mux=%x control=%x mux_global=%x irq_enabled=%u\n",
		name, snapshot->enable, snapshot->command, snapshot->source,
		snapshot->destination, snapshot->count, snapshot->mux,
		snapshot->control, snapshot->mux_global, snapshot->irq_enabled);
}

ssize_t raps_dma_test_status(struct dma_chan *dma_chan, char *buf, size_t size)
{
	struct raps_dma_chan *chan = raps_test_channel(dma_chan);
	struct raps_dma_snapshot live;
	struct raps_dma_test test;
	unsigned long flags;
	bool faulted;
	size_t used;

	if (!chan || !buf)
		return -EINVAL;
	if (!size)
		return 0;
	spin_lock_irqsave(&chan->vc.lock, flags);
	test = chan->test;
	faulted = chan->faulted;
	raps_snapshot(chan, &live);
	spin_unlock_irqrestore(&chan->vc.lock, flags);
	used = scnprintf(buf, size,
		"channel=%u irq=%d test_active=%u polling=%u faulted=%u "
		"starts=%u interrupts=%u completions=%u stops=%u stop_errors=%u\n",
		chan->index, chan->irq, test.active, test.polling, faulted,
		test.starts, test.interrupts, test.completions,
		test.stops, test.stop_errors);
	if (test.starts)
		used += raps_snapshot_format(buf + used, size - used,
					    "start", &test.start);
	if (test.interrupts)
		used += raps_snapshot_format(buf + used, size - used,
					    "first_irq", &test.first_irq);
	if (test.stops)
		used += raps_snapshot_format(buf + used, size - used,
					    "pre_stop", &test.pre_stop);
	used += raps_snapshot_format(buf + used, size - used, "live", &live);
	return used;
}
EXPORT_SYMBOL_GPL(raps_dma_test_status);

static struct dma_chan *raps_of_xlate(struct of_phandle_args *spec,
				    struct of_dma *ofdma)
{
	struct raps_dma *dma = ofdma->of_dma_data;
	struct dma_chan *chan;
	unsigned int i;

	/* Endpoint 18 in S40's table selects hardware request 9 (LCD TX). */
	if (spec->args_count != 1 || spec->args[0] != RAPS_LCD_REQUEST)
		return NULL;
	for (i = 0; i < RAPS_CHANNELS; i++) {
		if (READ_ONCE(dma->channels[i].faulted))
			continue;
		chan = dma_get_slave_channel(&dma->channels[i].vc.chan);
		if (chan)
			return chan;
	}
	return NULL;
}

static void raps_of_dma_free(void *node)
{
	of_dma_controller_free(node);
}

static int raps_dma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct raps_dma *dma;
	struct dma_device *engine;
	void __iomem *base, *mux, *control;
	unsigned int i;
	int ret;

	rm443_boot_stage(0x55);
	dma = devm_kzalloc(dev, sizeof(*dma), GFP_KERNEL);
	if (!dma)
		return -ENOMEM;
	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	mux = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(mux))
		return PTR_ERR(mux);
	control = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(control))
		return PTR_ERR(control);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	engine = &dma->device;
	engine->dev = dev;
	dma_cap_set(DMA_SLAVE, engine->cap_mask);
	dma_cap_set(DMA_PRIVATE, engine->cap_mask);
	engine->directions = BIT(DMA_MEM_TO_DEV);
	engine->src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	engine->dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	engine->residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR;
	engine->device_config = raps_config;
	engine->device_prep_slave_sg = raps_prep_slave_sg;
	engine->device_issue_pending = raps_issue_pending;
	engine->device_tx_status = raps_tx_status;
	engine->device_terminate_all = raps_terminate_all;
	engine->device_synchronize = raps_synchronize;
	engine->device_alloc_chan_resources = raps_alloc_resources;
	engine->device_free_chan_resources = raps_free_resources;
	INIT_LIST_HEAD(&engine->channels);
	for (i = 0; i < RAPS_CHANNELS; i++) {
		struct raps_dma_chan *chan = &dma->channels[i];

		chan->base = base + i * RAPS_STRIDE;
		chan->mux = mux + 4 + i * 4;
		chan->control = control;
		chan->mux_global = mux;
		chan->index = i;
		ret = raps_stop(chan);
		if (ret)
			return dev_err_probe(dev, ret, "DMA%u remains active\n", i);
		chan->vc.desc_free = raps_desc_free;
		vchan_init(&chan->vc, engine);
		chan->irq = platform_get_irq(pdev, i);
		if (chan->irq < 0)
			return chan->irq;
		ret = devm_request_irq(dev, chan->irq, raps_dma_interrupt,
				       IRQF_NO_AUTOEN,
				       "raps-dma", chan);
		if (ret)
			return ret;
	}
	/* Firmware's first-use setup, with all six channels stopped. */
	writel(0, mux);
	writel(0x100, control);
	readl(control);
	ret = dmaenginem_async_device_register(engine);
	if (ret)
		return ret;
	ret = of_dma_controller_register(dev->of_node, raps_of_xlate, dma);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, raps_of_dma_free, dev->of_node);
	if (ret)
		return ret;
	dev_info(dev, "RAPS DMA, %u channels, LCD request %u\n",
		 RAPS_CHANNELS, RAPS_LCD_REQUEST);
	rm443_boot_stage(0x56);
	return 0;
}

static const struct of_device_id raps_dma_match[] = {
	{ .compatible = "nokia,raps-dma" },
	{}
};

static struct platform_driver raps_dma_driver = {
	.probe = raps_dma_probe,
	.driver = {
		.name = "raps-dma",
		.of_match_table = raps_dma_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(raps_dma_driver);

MODULE_DESCRIPTION("RAPS linear DMA controller");
MODULE_LICENSE("GPL");
