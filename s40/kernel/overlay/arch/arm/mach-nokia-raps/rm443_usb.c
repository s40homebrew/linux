// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nokia 6303 classic USB full-speed device controller, single-buffer PIO.
 *
 * The RM-443 firmware's EP selection, FIFO, byte count, IRQ and endpoint
 * configuration follow the OMAP UDC register layout, with RAPS-specific
 * register access widths, clocks and PHY control.
 *
 * Device mode only. Remote wakeup, charging policy and clock shutdown are
 * not implemented.
 */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb/ch9.h>
#include <linux/usb/cdc.h>
#include <linux/usb/gadget.h>
#include <linux/workqueue.h>
#include "rm443_power.h"
#include "rm443_usb.h"

#define RM_EP_COUNT 7
#define RM_USB_BETTY 2
#define RM_USB_BETTY_USBR 6
#define RM_USB_BETTY_NSUSPEND BIT(1)
#define RM_USB_BETTY_SLAVE_SW BIT(2)

static unsigned int boot_wait_ms;
module_param(boot_wait_ms, uint, 0444);
MODULE_PARM_DESC(boot_wait_ms, "Wait for host ACM DTR at boot, milliseconds (0=off, maximum 30000)");

enum rm_ep0_state {
	RM_EP0_IDLE, RM_EP0_IN, RM_EP0_OUT, RM_EP0_STATUS_IN, RM_EP0_STATUS_OUT,
};

struct rm_usb;
struct rm_request {
	struct usb_request req;
	struct list_head queue;
};

struct rm_ep {
	struct usb_ep ep;
	struct rm_usb *usb;
	struct list_head queue;
	u8 address, type;
	bool enabled, halted, busy, last_packet, giving_back;
};

struct rm_usb {
	struct usb_gadget gadget;
	struct usb_gadget_driver *driver;
	struct device *dev;
	void __iomem *regs, *otg, *clocks, *semaphore, *pinmux;
	spinlock_t lock;
	struct delayed_work vbus_work;
	struct rm_ep eps[RM_EP_COUNT];
	enum rm_ep0_state ep0_state;
	struct usb_ctrlrequest last_setup;
	u16 setup_length, devstat;
	bool softconnect, vbus, removing, callbacks, fault, console_open;
	u32 setups, in_packets, out_packets, resets, errors;
	u32 irq_calls, in_acks;
};

static struct rm_ep *to_rm_ep(struct usb_ep *ep)
{
	return container_of(ep, struct rm_ep, ep);
}

static struct rm_usb *to_rm_usb(struct usb_gadget *gadget)
{
	return container_of(gadget, struct rm_usb, gadget);
}

static u16 rm_read(struct rm_usb *usb, unsigned int reg)
{
	/* RAPS exposes word-wide control registers, even though their fields
	 * match the 16-bit OMAP UDC. S40 uses LDR/STR here, never LDRH/STRH. */
	return readl(usb->regs + reg);
}

static void rm_write(struct rm_usb *usb, unsigned int reg, u16 value)
{
	writel(value, usb->regs + reg);
}

static u16 ep_selector(u8 address)
{
	return (address & 15) | ((address & USB_DIR_IN) ? RM_USB_EP_DIR : 0);
}

static struct rm_ep *find_ep(struct rm_usb *usb, u16 address)
{
	unsigned int i;
	if (address & ~0x8f)
		return NULL;
	if (!(address & 15))
		return &usb->eps[0];
	for (i = 1; i < RM_EP_COUNT; ++i)
		if (usb->eps[i].address == address)
			return &usb->eps[i];
	return NULL;
}

static struct rm_request *first_request(struct rm_ep *ep)
{
	return list_first_entry_or_null(&ep->queue, struct rm_request, queue);
}

static void select_ep(struct rm_usb *usb, u8 address, bool fifo)
{
	rm_write(usb, RM_USB_EP_NUM,
		 ep_selector(address) | (fifo ? RM_USB_EP_SEL : 0));
}

/* S40's EP0 path uses byte FIFO accesses. Its non-control word accesses move
 * only TWO bytes per LDR/STR, unlike normal RAM or our current emulator.
 * Use the verified byte path throughout; never infer FIFO width from the
 * control register bus width. All transfers are bounded by maxpacket. */
static void write_packet(struct rm_usb *usb, u8 address, const u8 *data,
			 unsigned int count)
{
	unsigned int i;
	select_ep(usb, address, true);
	for (i = 0; i < count; ++i)
		writeb(data[i], usb->regs + RM_USB_DATA);
	/* Firmware deselects before FIFO_EN. The controller latches this packet
	 * only after releasing EP_SEL; retain the direction/endpoint number. */
	select_ep(usb, address, false);
	rm_write(usb, RM_USB_CTRL, RM_USB_FIFO_EN);
	++usb->in_packets;
}

static void arm_out(struct rm_usb *usb, u8 address)
{
	select_ep(usb, address, false);
	rm_write(usb, RM_USB_CTRL, RM_USB_FIFO_EN);
}

static void clear_fifo(struct rm_usb *usb, u8 address)
{
	select_ep(usb, address, false);
	rm_write(usb, RM_USB_CTRL, RM_USB_CLR_EP);
}

static void giveback(struct rm_ep *ep, struct rm_request *req, int status)
{
	list_del_init(&req->queue);
	if (req->req.status == -EINPROGRESS)
		req->req.status = status;
	ep->giving_back = true;
	spin_unlock(&ep->usb->lock);
	usb_gadget_giveback_request(&ep->ep, &req->req);
	spin_lock(&ep->usb->lock);
	ep->giving_back = false;
}

static void nuke(struct rm_ep *ep, int status)
{
	struct rm_request *req;
	ep->busy = false;
	clear_fifo(ep->usb, ep->address);
	while ((req = first_request(ep)))
		giveback(ep, req, status);
}

static void stall_ep0(struct rm_usb *usb)
{
	rm_write(usb, RM_USB_SYSCON2, RM_USB_STALL);
	usb->ep0_state = RM_EP0_IDLE;
	usb->eps[0].busy = false;
	++usb->errors;
}

static void kick(struct rm_ep *ep)
{
	struct rm_usb *usb = ep->usb;
	struct rm_request *req = first_request(ep);
	unsigned int count;
	bool in = ep->address & USB_DIR_IN;
	u8 address = ep->address;
	if (!req || !ep->enabled || ep->halted || ep->busy || ep->giving_back)
		return;
	if (!address) {
		in = usb->ep0_state == RM_EP0_IN ||
			usb->ep0_state == RM_EP0_STATUS_IN;
		if (usb->ep0_state == RM_EP0_IDLE)
			return;
		address = in ? USB_DIR_IN : 0;
	}
	ep->busy = true;
	if (!in) {
		arm_out(usb, address);
		return;
	}
	count = min_t(unsigned int, req->req.length - req->req.actual,
		      ep->ep.maxpacket);
	ep->last_packet = count < ep->ep.maxpacket ||
		(req->req.actual + count == req->req.length && !req->req.zero);
	/* A control response exactly filling wLength needs no terminating ZLP. */
	if (!ep->address && req->req.actual + count == usb->setup_length)
		ep->last_packet = true;
	write_packet(usb, address, count ? (u8 *)req->req.buf + req->req.actual : NULL, count);
	req->req.actual += count;
}

static int rm_ep_enable(struct usb_ep *endpoint,
			const struct usb_endpoint_descriptor *desc)
{
	struct rm_ep *ep = to_rm_ep(endpoint);
	struct rm_usb *usb = ep->usb;
	unsigned long flags;
	unsigned int maxpacket = usb_endpoint_maxp(desc);
	if (!ep->address || desc->bDescriptorType != USB_DT_ENDPOINT ||
	    desc->bEndpointAddress != ep->address ||
	    usb_endpoint_type(desc) != ep->type || !maxpacket ||
	    maxpacket > endpoint->maxpacket_limit ||
	    (ep->type == USB_ENDPOINT_XFER_BULK && maxpacket != 64))
		return -EINVAL;
	spin_lock_irqsave(&usb->lock, flags);
	if (!usb->driver || usb->gadget.speed == USB_SPEED_UNKNOWN) {
		spin_unlock_irqrestore(&usb->lock, flags);
		return -ESHUTDOWN;
	}
	endpoint->desc = desc;
	endpoint->maxpacket = maxpacket;
	ep->enabled = true;
	ep->halted = false;
	ep->busy = false;
	select_ep(usb, ep->address, false);
	rm_write(usb, RM_USB_CTRL, RM_USB_CLR_EP | RM_USB_CLR_HALT | RM_USB_CLR_TOGGLE);
	spin_unlock_irqrestore(&usb->lock, flags);
	return 0;
}

static int rm_ep_disable(struct usb_ep *endpoint)
{
	struct rm_ep *ep = to_rm_ep(endpoint);
	unsigned long flags;
	if (!ep->address)
		return -EINVAL;
	spin_lock_irqsave(&ep->usb->lock, flags);
	ep->enabled = false;
	nuke(ep, -ESHUTDOWN);
	endpoint->desc = NULL;
	spin_unlock_irqrestore(&ep->usb->lock, flags);
	return 0;
}

static struct usb_request *rm_alloc_request(struct usb_ep *ep, gfp_t flags)
{
	struct rm_request *req = kzalloc(sizeof(*req), flags);
	if (!req)
		return NULL;
	INIT_LIST_HEAD(&req->queue);
	return &req->req;
}

static void rm_free_request(struct usb_ep *ep, struct usb_request *request)
{
	struct rm_request *req = container_of(request, struct rm_request, req);
	WARN_ON(!list_empty(&req->queue));
	kfree(req);
}

static int rm_queue(struct usb_ep *endpoint, struct usb_request *request, gfp_t flags)
{
	struct rm_ep *ep = to_rm_ep(endpoint);
	struct rm_usb *usb = ep->usb;
	struct rm_request *req;
	unsigned long irqflags;
	int error = 0;
	if (!request || !request->complete || (!request->buf && request->length))
		return -EINVAL;
	req = container_of(request, struct rm_request, req);
	spin_lock_irqsave(&usb->lock, irqflags);
	if (!list_empty(&req->queue))
		error = -EBUSY;
	else if (!ep->enabled || !usb->driver || usb->fault ||
		 usb->gadget.speed == USB_SPEED_UNKNOWN)
		error = -ESHUTDOWN;
	else if (!ep->address && (usb->ep0_state == RM_EP0_IDLE ||
		 !list_empty(&ep->queue) || request->length > usb->setup_length))
		error = -EINVAL;
	if (!error) {
		request->actual = 0;
		request->status = -EINPROGRESS;
		list_add_tail(&req->queue, &ep->queue);
		kick(ep);
	}
	spin_unlock_irqrestore(&usb->lock, irqflags);
	return error;
}

static int rm_dequeue(struct usb_ep *endpoint, struct usb_request *request)
{
	struct rm_ep *ep = to_rm_ep(endpoint);
	struct rm_request *req;
	unsigned long flags;
	int result = -EINVAL;
	spin_lock_irqsave(&ep->usb->lock, flags);
	list_for_each_entry(req, &ep->queue, queue) {
		if (&req->req != request)
			continue;
		if (req == first_request(ep)) {
			clear_fifo(ep->usb, ep->address);
			ep->busy = false;
		}
		giveback(ep, req, -ECONNRESET);
		kick(ep);
		result = 0;
		break;
	}
	spin_unlock_irqrestore(&ep->usb->lock, flags);
	return result;
}

static int set_halt_locked(struct rm_ep *ep, bool halt)
{
	if (!ep->address) {
		if (halt)
			stall_ep0(ep->usb);
		return 0;
	}
	if (!ep->enabled)
		return -EINVAL;
	if (halt && (ep->address & USB_DIR_IN) && ep->busy)
		return -EAGAIN;
	ep->halted = halt;
	select_ep(ep->usb, ep->address, false);
	rm_write(ep->usb, RM_USB_CTRL, halt ? RM_USB_SET_HALT :
		 RM_USB_CLR_HALT | RM_USB_CLR_TOGGLE);
	if (!halt) {
		ep->busy = false;
		kick(ep);
	}
	return 0;
}

static int rm_set_halt(struct usb_ep *endpoint, int value)
{
	struct rm_ep *ep = to_rm_ep(endpoint);
	unsigned long flags;
	int error;
	spin_lock_irqsave(&ep->usb->lock, flags);
	error = set_halt_locked(ep, value);
	spin_unlock_irqrestore(&ep->usb->lock, flags);
	return error;
}

static void rm_fifo_flush(struct usb_ep *endpoint)
{
	struct rm_ep *ep = to_rm_ep(endpoint);
	unsigned long flags;
	spin_lock_irqsave(&ep->usb->lock, flags);
	if (list_empty(&ep->queue)) {
		clear_fifo(ep->usb, ep->address);
		ep->busy = false;
	}
	spin_unlock_irqrestore(&ep->usb->lock, flags);
}

static const struct usb_ep_ops rm_ep_ops = {
	.enable = rm_ep_enable, .disable = rm_ep_disable,
	.alloc_request = rm_alloc_request, .free_request = rm_free_request,
	.queue = rm_queue, .dequeue = rm_dequeue, .set_halt = rm_set_halt,
	.fifo_flush = rm_fifo_flush,
};

static void status_in(struct rm_usb *usb)
{
	usb->ep0_state = RM_EP0_STATUS_IN;
	usb->eps[0].busy = true;
	clear_fifo(usb, USB_DIR_IN);
	write_packet(usb, USB_DIR_IN, NULL, 0);
}

static void status_out(struct rm_usb *usb)
{
	usb->ep0_state = RM_EP0_STATUS_OUT;
	usb->eps[0].busy = true;
	clear_fifo(usb, 0);
	arm_out(usb, 0);
}

static void handle_in(struct rm_ep *ep)
{
	struct rm_usb *usb = ep->usb;
	struct rm_request *req = first_request(ep);
	select_ep(usb, ep->address | USB_DIR_IN, true);
	if (!(rm_read(usb, RM_USB_STAT) & RM_USB_ACK)) {
		select_ep(usb, ep->address | USB_DIR_IN, false);
		return;
	}
	select_ep(usb, ep->address | USB_DIR_IN, false);
	++usb->in_acks;
	ep->busy = false;
	if (!ep->address && usb->ep0_state == RM_EP0_STATUS_IN) {
		usb->ep0_state = RM_EP0_IDLE;
		if (req)
			giveback(ep, req, 0);
		return;
	}
	if (req && ep->last_packet) {
		giveback(ep, req, 0);
		if (!ep->address && usb->ep0_state == RM_EP0_IN)
			status_out(usb);
	} else if (!req && !ep->address && usb->ep0_state == RM_EP0_IN) {
		/* An internally answered GET_STATUS has no gadget request. */
		status_out(usb);
	}
	kick(ep);
}

static void handle_out(struct rm_ep *ep)
{
	struct rm_usb *usb = ep->usb;
	struct rm_request *req = first_request(ep);
	unsigned int count, i, available;
	u8 *data;
	select_ep(usb, ep->address, true);
	if (!(rm_read(usb, RM_USB_STAT) & RM_USB_ACK)) {
		select_ep(usb, ep->address, false);
		return;
	}
	available = rm_read(usb, RM_USB_RX_COUNT);
	if (available > ep->ep.maxpacket) {
		clear_fifo(usb, ep->address);
		ep->busy = false;
		++usb->errors;
		if (req)
			giveback(ep, req, -EOVERFLOW);
		if (!ep->address)
			stall_ep0(usb);
		else
			kick(ep);
		return;
	}
	/* No queued OUT request means leave the packet latched/backpressured.
	 * This can occur if the host clears a halt while the function is idle. */
	if (!req && (ep->address || usb->ep0_state != RM_EP0_STATUS_OUT)) {
		select_ep(usb, ep->address, false);
		return;
	}
	count = req ? min(available, req->req.length - req->req.actual) : 0;
	data = req ? (u8 *)req->req.buf + req->req.actual : NULL;
	for (i = 0; i < available; ++i) {
		u8 byte = readb(usb->regs + RM_USB_DATA);
		if (i < count)
			data[i] = byte;
	}
	select_ep(usb, ep->address, false);
	ep->busy = false;
	++usb->out_packets;
	if (!ep->address && usb->ep0_state == RM_EP0_STATUS_OUT) {
		usb->ep0_state = RM_EP0_IDLE;
		if (available)
			stall_ep0(usb);
		return;
	}
	req->req.actual += count;
	if (count < available || available < ep->ep.maxpacket ||
	    req->req.actual == req->req.length) {
		int result = count < available ? -EOVERFLOW : 0;
		if (!result && req->req.short_not_ok &&
		    req->req.actual < req->req.length)
			result = -EREMOTEIO;
		giveback(ep, req, result);
		if (!ep->address && usb->ep0_state == RM_EP0_OUT) {
			if (result)
				stall_ep0(usb);
			else
				status_in(usb);
		}
	}
	kick(ep);
}

static void rm_track_console_open(struct rm_usb *usb,
				 const struct usb_ctrlrequest *setup, int result)
{
	if (result < 0)
		return;
	/* The function driver has validated the ACM interface number. Do not
	 * mistake enumeration or another function's rejected request for a host
	 * opening the serial port. The probe wait reads this from process context. */
	if (setup->bRequestType == (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) &&
	    setup->bRequest == USB_CDC_REQ_SET_CONTROL_LINE_STATE && !setup->wLength)
		WRITE_ONCE(usb->console_open,
			   !!(le16_to_cpu(setup->wValue) & USB_CDC_CTRL_DTR));
	else if (setup->bRequestType == USB_RECIP_DEVICE &&
		 setup->bRequest == USB_REQ_SET_CONFIGURATION && !setup->wValue)
		WRITE_ONCE(usb->console_open, false);
}

static void handle_setup(struct rm_usb *usb)
{
	struct rm_ep *ep0 = &usb->eps[0], *ep;
	struct usb_ctrlrequest packet;
	struct usb_ctrlrequest *setup = &packet;
	u16 value, index, length;
	unsigned int i;
	int result = -EOPNOTSUPP;
	bool configuration;
	/* Reject callback requeueing of the superseded control transfer. */
	usb->ep0_state = RM_EP0_IDLE;
	nuke(ep0, -ECONNRESET);
	clear_fifo(usb, USB_DIR_IN);
	rm_write(usb, RM_USB_IRQ_SRC, RM_USB_IRQ_EP0_RX | RM_USB_IRQ_EP0_TX);
	rm_write(usb, RM_USB_EP_NUM, RM_USB_SETUP_SEL);
	for (i = 0; i < sizeof(packet); ++i)
		((u8 *)&packet)[i] = readb(usb->regs + RM_USB_DATA);
	rm_write(usb, RM_USB_EP_NUM, 0);
	/* F01B0946 clears EP0 OUT FIFO_EN after consuming SETUP, before
	 * dispatching its request. Do not leave an old OUT stage armed. */
	rm_write(usb, RM_USB_CTRL, rm_read(usb, RM_USB_CTRL) & ~RM_USB_FIFO_EN);
	usb->last_setup = packet;
	value = le16_to_cpu(setup->wValue);
	index = le16_to_cpu(setup->wIndex);
	length = le16_to_cpu(setup->wLength);
	usb->setup_length = length;
	usb->ep0_state = !length ? RM_EP0_STATUS_IN :
		(setup->bRequestType & USB_DIR_IN) ? RM_EP0_IN : RM_EP0_OUT;
	ep0->halted = false;
	++usb->setups;
	configuration = setup->bRequestType == USB_RECIP_DEVICE &&
		setup->bRequest == USB_REQ_SET_CONFIGURATION;
	if ((setup->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD) {
		if (setup->bRequest == USB_REQ_GET_STATUS &&
		    (setup->bRequestType & USB_DIR_IN) && !value && length == 2) {
			u8 reply[2] = {0, 0};
			switch (setup->bRequestType & USB_RECIP_MASK) {
			case USB_RECIP_DEVICE:
				if (index)
					goto stalled;
				reply[0] = usb->gadget.is_selfpowered ? 1 : 0;
				break;
			case USB_RECIP_ENDPOINT:
				ep = find_ep(usb, index);
				if (!ep || !ep->enabled)
					goto stalled;
				reply[0] = ep->halted;
				break;
			case USB_RECIP_INTERFACE:
				/* Full-speed interfaces have no status flags. As with
				 * omap_udc, the UDC API does not expose the function's
				 * interface table; reject nonconfigured/high-byte indices. */
				if (index > 255 || !(rm_read(usb, RM_USB_DEVSTAT) & BIT(3)))
					goto stalled;
				break;
			default:
				goto delegate;
			}
			ep0->busy = true;
			write_packet(usb, USB_DIR_IN, reply, sizeof(reply));
			return;
		}
		if ((setup->bRequest == USB_REQ_CLEAR_FEATURE ||
		     setup->bRequest == USB_REQ_SET_FEATURE) &&
		    setup->bRequestType == USB_RECIP_ENDPOINT &&
		    value == USB_ENDPOINT_HALT && !length) {
			ep = find_ep(usb, index);
			if (!ep || !ep->address)
				goto stalled;
			result = set_halt_locked(ep, setup->bRequest == USB_REQ_SET_FEATURE);
			if (result)
				goto stalled;
			status_in(usb);
			return;
		}
	}
delegate:
	if (configuration) {
		if (length || index || value > 255)
			goto stalled;
		rm_write(usb, RM_USB_SYSCON2,
			 value ? RM_USB_CONFIGURED : RM_USB_CLEAR_CONFIG);
	}
	if (usb->driver && usb->callbacks) {
		spin_unlock(&usb->lock);
		result = usb->driver->setup(&usb->gadget, setup);
		spin_lock(&usb->lock);
	}
	rm_track_console_open(usb, setup, result);
	if (result >= 0)
		return;
stalled:
	if (configuration)
		rm_write(usb, RM_USB_SYSCON2, RM_USB_CLEAR_CONFIG);
	usb->ep0_state = RM_EP0_IDLE;
	nuke(ep0, -EPIPE);
	stall_ep0(usb);
}

static void disconnect_locked(struct rm_usb *usb, bool reset)
{
	unsigned int i;
	struct usb_gadget_driver *driver = usb->driver;
	WRITE_ONCE(usb->console_open, false);
	usb->gadget.speed = USB_SPEED_UNKNOWN;
	usb->ep0_state = RM_EP0_IDLE;
	for (i = 0; i < RM_EP_COUNT; ++i) {
		if (i)
			usb->eps[i].enabled = false;
		nuke(&usb->eps[i], -ESHUTDOWN);
	}
	clear_fifo(usb, USB_DIR_IN);
	if (driver && usb->callbacks) {
		spin_unlock(&usb->lock);
		if (reset)
			usb_gadget_udc_reset(&usb->gadget, driver);
		else if (driver->disconnect)
			driver->disconnect(&usb->gadget);
		spin_lock(&usb->lock);
	}
}

static void handle_state(struct rm_usb *usb)
{
	u16 state = rm_read(usb, RM_USB_DEVSTAT), previous = usb->devstat;
	usb->devstat = state;
	rm_write(usb, RM_USB_IRQ_SRC, RM_USB_IRQ_STATE);
	if ((state & RM_USB_RESET) && !(previous & RM_USB_RESET)) {
		rm_write(usb, RM_USB_IRQ_EN,
			 usb->driver && !usb->fault && !usb->removing ? RM_USB_IRQ_STATE : 0);
		++usb->resets;
		disconnect_locked(usb, true);
	} else if (!(state & RM_USB_ATTACHED) && (previous & RM_USB_ATTACHED)) {
		rm_write(usb, RM_USB_IRQ_EN,
			 usb->driver && !usb->fault && !usb->removing ? RM_USB_IRQ_STATE : 0);
		disconnect_locked(usb, false);
	}
	if ((state & RM_USB_ATTACHED) && !(state & RM_USB_RESET)) {
		usb->gadget.speed = USB_SPEED_FULL;
		/* Bus reset can leave only DS_CHG enabled (physical 0.7.5:
		 * IRQ_EN=0008, SETUP pending, zero requests handled). Re-enable
		 * EP0 and PIO endpoint traffic after reset, as F01B0600 and
		 * omap_udc's devstate_irq do. Never clear a pending SETUP here. */
		if (usb->driver && !usb->fault && !usb->removing &&
		    rm_read(usb, RM_USB_IRQ_EN) != RM_USB_IRQ_ENABLE)
			rm_write(usb, RM_USB_IRQ_EN, RM_USB_IRQ_ENABLE);
	}
	if (usb->driver && usb->callbacks && ((state ^ previous) & RM_USB_SUSPEND)) {
		void (*callback)(struct usb_gadget *) = (state & RM_USB_SUSPEND) ?
			usb->driver->suspend : usb->driver->resume;
		if (callback) {
			spin_unlock(&usb->lock);
			callback(&usb->gadget);
			spin_lock(&usb->lock);
		}
	}
}

static u16 rm_pending_irq(struct rm_usb *usb)
{
	u16 enable = rm_read(usb, RM_USB_IRQ_EN);
	u16 mask = enable & (RM_USB_IRQ_STATE | RM_USB_IRQ_TX | RM_USB_IRQ_RX);
	/* IRQ_EN bit 0 gates all three EP0 sources, not just EP0_TX. SOF and
	 * DMA are disabled; their raw pending latches must not drive our loop. */
	if (enable & BIT(0))
		mask |= RM_USB_IRQ_EP0_TX | RM_USB_IRQ_EP0_RX | RM_USB_IRQ_SETUP;
	return rm_read(usb, RM_USB_IRQ_SRC) & mask;
}

static irqreturn_t rm_usb_irq(int irq, void *data)
{
	struct rm_usb *usb = data;
	unsigned long flags;
	unsigned int budget = 32;
	u16 source;
	bool handled = false;
	spin_lock_irqsave(&usb->lock, flags);
	++usb->irq_calls;
	while ((source = rm_pending_irq(usb)) && budget--) {
		struct rm_ep *ep;
		u16 endpoints;
		handled = true;
		if (source & RM_USB_IRQ_STATE) {
			handle_state(usb);
			continue;
		}
		if (source & RM_USB_IRQ_SETUP) {
			handle_setup(usb);
			continue;
		}
		if (source & RM_USB_IRQ_EP0_TX) {
			rm_write(usb, RM_USB_IRQ_SRC, RM_USB_IRQ_EP0_TX);
			handle_in(&usb->eps[0]);
		}
		if (source & RM_USB_IRQ_EP0_RX) {
			rm_write(usb, RM_USB_IRQ_SRC, RM_USB_IRQ_EP0_RX);
			handle_out(&usb->eps[0]);
		}
		/* Capture EPN_STAT before its source acknowledgement advances the
		 * hardware's pending-endpoint selector. */
		endpoints = rm_read(usb, RM_USB_EPN_STAT);
		if (source & RM_USB_IRQ_TX) {
			rm_write(usb, RM_USB_IRQ_SRC, RM_USB_IRQ_TX);
			ep = find_ep(usb, USB_DIR_IN | (endpoints & 15));
			if (ep && ep->address)
				handle_in(ep);
		}
		if (source & RM_USB_IRQ_RX) {
			rm_write(usb, RM_USB_IRQ_SRC, RM_USB_IRQ_RX);
			ep = find_ep(usb, (endpoints >> 8) & 15);
			if (ep && ep->address)
				handle_out(ep);
		}
	}
	if (source) {
		usb->fault = true;
		rm_write(usb, RM_USB_IRQ_EN, 0);
		++usb->errors;
		dev_err_ratelimited(usb->dev, "USB interrupt did not clear (%04x); masked\n", source);
	}
	spin_unlock_irqrestore(&usb->lock, flags);
	return handled ? IRQ_HANDLED : IRQ_NONE;
}

static int apply_pullup(struct rm_usb *usb)
{
	u16 config = rm_read(usb, RM_USB_SYSCON1);
	u32 otg = readl(usb->otg + RM_OTG_CTRL);
	bool connect = usb->driver && usb->softconnect && usb->vbus && !usb->fault;
	int error;

	if (connect) {
		/* F01AF3AE: wake the Betty PHY before the mux/controller pullup,
		 * then close its separate peripheral switch. UDC_PULLUP alone
		 * never closes that electrical path on the physical phone. */
		error = rm443_cbus_update(RM_USB_BETTY, RM_USB_BETTY_USBR,
			RM_USB_BETTY_NSUSPEND, RM_USB_BETTY_NSUSPEND);
		if (error)
			goto failed;
		udelay(5); /* F037C7D8 settles each Betty field update. */
		/* Firmware selects the USB mux with 0xff. Preserve any unrelated
		 * upper fields rather than copying its whole-register overwrite. */
		writel(readl(usb->pinmux) | 0xff, usb->pinmux);
		otg |= RM_OTG_BSESSVLD;
		config |= RM_USB_PULLUP;
		writel(otg, usb->otg + RM_OTG_CTRL);
		rm_write(usb, RM_USB_SYSCON1, config);
		error = rm443_cbus_update(RM_USB_BETTY, RM_USB_BETTY_USBR,
			RM_USB_BETTY_SLAVE_SW, RM_USB_BETTY_SLAVE_SW);
		if (error)
			goto failed;
	} else {
		config &= ~RM_USB_PULLUP;
		otg &= ~RM_OTG_BSESSVLD;
		rm_write(usb, RM_USB_SYSCON1, config);
		writel(otg, usb->otg + RM_OTG_CTRL);
		error = rm443_cbus_update(RM_USB_BETTY, RM_USB_BETTY_USBR,
			RM_USB_BETTY_SLAVE_SW, 0);
		if (error)
			goto failed;
		udelay(5);
		error = rm443_cbus_update(RM_USB_BETTY, RM_USB_BETTY_USBR,
			RM_USB_BETTY_NSUSPEND, 0);
		if (error)
			goto failed;
	}
	return 0;
failed:
	usb->fault = true;
	++usb->errors;
	rm_write(usb, RM_USB_SYSCON1, config & ~RM_USB_PULLUP);
	writel(otg & ~RM_OTG_BSESSVLD, usb->otg + RM_OTG_CTRL);
	rm_write(usb, RM_USB_IRQ_EN, 0);
	dev_err_ratelimited(usb->dev, "USB PHY %s failed: %d; pullup disabled\n",
		connect ? "connect" : "disconnect", error);
	return error;
}

static int rm_pullup(struct usb_gadget *gadget, int connect)
{
	struct rm_usb *usb = to_rm_usb(gadget);
	unsigned long flags;
	int error;
	spin_lock_irqsave(&usb->lock, flags);
	usb->softconnect = connect;
	error = apply_pullup(usb);
	spin_unlock_irqrestore(&usb->lock, flags);
	return error;
}

static int rm_vbus_session(struct usb_gadget *gadget, int active)
{
	struct rm_usb *usb = to_rm_usb(gadget);
	unsigned long flags;
	int error;
	spin_lock_irqsave(&usb->lock, flags);
	usb->vbus = active;
	error = apply_pullup(usb);
	if (!active && gadget->speed != USB_SPEED_UNKNOWN)
		disconnect_locked(usb, false);
	spin_unlock_irqrestore(&usb->lock, flags);
	return error;
}

static int rm_selfpowered(struct usb_gadget *gadget, int powered)
{
	struct rm_usb *usb = to_rm_usb(gadget);
	unsigned long flags;
	u16 config;
	spin_lock_irqsave(&usb->lock, flags);
	gadget->is_selfpowered = !!powered;
	config = rm_read(usb, RM_USB_SYSCON1);
	config = powered ? config | RM_USB_SELF_POWERED : config & ~RM_USB_SELF_POWERED;
	rm_write(usb, RM_USB_SYSCON1, config);
	spin_unlock_irqrestore(&usb->lock, flags);
	return 0;
}

static int rm_get_frame(struct usb_gadget *gadget)
{
	u16 frame = rm_read(to_rm_usb(gadget), RM_USB_SOF);
	/* OMAP TS_OK distinguishes an observed SOF from an unclocked register. */
	return frame & BIT(11) ? frame & 0x3ff : -EL2NSYNC;
}

static int rm_udc_start(struct usb_gadget *gadget, struct usb_gadget_driver *driver)
{
	struct rm_usb *usb = to_rm_usb(gadget);
	unsigned long flags;
	spin_lock_irqsave(&usb->lock, flags);
	usb->driver = driver;
	usb->fault = false;
	usb->eps[0].enabled = true;
	rm_write(usb, RM_USB_IRQ_SRC, RM_USB_IRQ_MASK);
	rm_write(usb, RM_USB_IRQ_EN, RM_USB_IRQ_ENABLE);
	spin_unlock_irqrestore(&usb->lock, flags);
	return 0;
}

static int rm_udc_stop(struct usb_gadget *gadget)
{
	struct rm_usb *usb = to_rm_usb(gadget);
	unsigned long flags;
	int error;
	spin_lock_irqsave(&usb->lock, flags);
	rm_write(usb, RM_USB_IRQ_EN, 0);
	usb->softconnect = false;
	error = apply_pullup(usb);
	disconnect_locked(usb, false);
	usb->driver = NULL;
	spin_unlock_irqrestore(&usb->lock, flags);
	return error;
}

static void rm_async_callbacks(struct usb_gadget *gadget, bool enable)
{
	struct rm_usb *usb = to_rm_usb(gadget);
	unsigned long flags;
	spin_lock_irqsave(&usb->lock, flags);
	usb->callbacks = enable;
	spin_unlock_irqrestore(&usb->lock, flags);
}

static const struct usb_gadget_ops rm_gadget_ops = {
	.get_frame = rm_get_frame, .set_selfpowered = rm_selfpowered,
	.pullup = rm_pullup, .vbus_session = rm_vbus_session,
	.udc_start = rm_udc_start, .udc_stop = rm_udc_stop,
	.udc_async_callbacks = rm_async_callbacks,
};

static void rm_vbus_poll(struct work_struct *work)
{
	struct rm_usb *usb = container_of(to_delayed_work(work), struct rm_usb, vbus_work);
	u16 status;
	int error = rm443_cbus_read(RM_USB_BETTY, 2, &status);
	if (!error) {
		bool active = status & 1;
		if (active != READ_ONCE(usb->vbus)) {
			if (active)
				usb_gadget_vbus_connect(&usb->gadget);
			else
				usb_gadget_vbus_disconnect(&usb->gadget);
			dev_info(usb->dev, "USB VBUS %s\n", active ? "present" : "absent");
		}
	}
	/* A failed PMIC read is not evidence that the cable was removed. */
	if (!READ_ONCE(usb->removing))
		schedule_delayed_work(&usb->vbus_work, msecs_to_jiffies(100));
}

static int rm_usb_hardware_init(struct rm_usb *usb)
{
	u32 value;
	unsigned int tries;
	int error;
	/* Firmware resource3 takes hardware semaphore2, enables PLL gate bit15
	 * and waits for status bit0. Do not alter its frequency or later gate
	 * shared clocks off using S40's now-invalid software reference counts. */
	for (tries = 0; tries < 100000; ++tries)
		if ((s32)readl(usb->semaphore) > 0)
			break;
	if (tries == 100000)
		return -EBUSY;
	writel(0x8000, usb->clocks + 0x08);
	error = readl_poll_timeout(usb->clocks + 0x100, value, value & 1, 10, 10000);
	writel(U32_MAX, usb->semaphore);
	if (error)
		return error;
	/* Resource4 enable and the USB-specific gate from F01AFF24. */
	writel(1, usb->clocks + 0x54);
	writel(0x10, usb->clocks + 0x04);
	writel(0x400, usb->clocks + 0x34);
	error = rm443_cbus_update(RM_USB_BETTY, 6, 0x18, 0);
	if (error)
		return error;
	writel(RM_OTG_RESET, usb->otg + RM_OTG_SYSCON1);
	error = readl_poll_timeout(usb->otg + RM_OTG_SYSCON1, value,
				   value & RM_OTG_RESET_DONE, 10, 10000);
	if (error)
		return error;
	writel(0x3e000, usb->otg + RM_OTG_SYSCON1);
	writel(0x40000000, usb->otg + RM_OTG_SYSCON2);
	error = rm443_cbus_update(RM_USB_BETTY, 6, 0x147, 0);
	if (error)
		return error;
	/* Normal attach is F01AF3AE, implemented by apply_pullup(). F01B027E
	 * instead prepares the PHY self-tests F01B02CE/F01B023A. Its UDC+0x78
	 * override and Betty interrupt-mask writes do not belong in attach. */
	return 0;
}

static void rm_init_endpoints(struct rm_usb *usb)
{
	static const struct {
		const char *name;
		u8 address, type, size;
	} layout[RM_EP_COUNT] = {
		{ "ep0", 0, USB_ENDPOINT_XFER_CONTROL, 64 },
		{ "ep1in-bulk", 0x81, USB_ENDPOINT_XFER_BULK, 64 },
		{ "ep2out-bulk", 0x02, USB_ENDPOINT_XFER_BULK, 64 },
		{ "ep3in-int", 0x83, USB_ENDPOINT_XFER_INT, 16 },
		{ "ep4in-bulk", 0x84, USB_ENDPOINT_XFER_BULK, 64 },
		{ "ep5out-bulk", 0x05, USB_ENDPOINT_XFER_BULK, 64 },
		{ "ep6in-int", 0x86, USB_ENDPOINT_XFER_INT, 16 },
	};
	unsigned int i, offset = 8; /* SETUP owns the first eight FIFO bytes. */
	rm_write(usb, RM_USB_SYSCON1, 0);
	rm_write(usb, RM_USB_IRQ_EN, 0);
	rm_write(usb, RM_USB_DMA_IRQ_EN, 0);
	rm_write(usb, RM_USB_RX_DMA, 0);
	rm_write(usb, RM_USB_TX_DMA, 0);
	rm_write(usb, RM_USB_IRQ_SRC, RM_USB_IRQ_MASK);
	for (i = 1; i < 16; ++i) {
		rm_write(usb, RM_USB_EP_RX(i), 0);
		rm_write(usb, RM_USB_EP_TX(i), 0);
	}
	INIT_LIST_HEAD(&usb->gadget.ep_list);
	for (i = 0; i < RM_EP_COUNT; ++i) {
		struct rm_ep *ep = &usb->eps[i];
		u16 config = (layout[i].size == 64 ? 3 : 1) << 12;
		ep->usb = usb;
		ep->address = layout[i].address;
		ep->type = layout[i].type;
		ep->ep.name = layout[i].name;
		ep->ep.ops = &rm_ep_ops;
		ep->ep.address = ep->address;
		ep->ep.caps.type_control = !i;
		ep->ep.caps.type_bulk = ep->type == USB_ENDPOINT_XFER_BULK;
		ep->ep.caps.type_int = ep->type == USB_ENDPOINT_XFER_INT;
		ep->ep.caps.dir_in = !i || (ep->address & USB_DIR_IN);
		ep->ep.caps.dir_out = !i || !(ep->address & USB_DIR_IN);
		usb_ep_set_maxpacket_limit(&ep->ep, layout[i].size);
		INIT_LIST_HEAD(&ep->queue);
		INIT_LIST_HEAD(&ep->ep.ep_list);
		if (i) {
			config |= RM_USB_EP_VALID;
			list_add_tail(&ep->ep.ep_list, &usb->gadget.ep_list);
		}
		config |= offset >> 3;
		rm_write(usb, ep->address & USB_DIR_IN ? RM_USB_EP_TX(ep->address & 15) :
			 RM_USB_EP_RX(ep->address), config);
		offset += layout[i].size;
	}
	usb->gadget.ep0 = &usb->eps[0].ep;
	usb->eps[0].enabled = true;
	/* Keep SET_ADDRESS and device requests decoded by the controller. */
	rm_write(usb, RM_USB_SYSCON1, RM_USB_CFG_LOCK | RM_USB_SELF_POWERED);
}

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rm_usb *usb = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t length;
	u16 betty_control = 0;
	int error = rm443_cbus_read(RM_USB_BETTY, RM_USB_BETTY_USBR, &betty_control);
	spin_lock_irqsave(&usb->lock, flags);
	length = sysfs_emit(buf,
		"USB controller status\n"
		"vbus=%u pullup=%u speed=%u state=%04x irq=%04x fault=%u\n"
		"betty6=%04x cbus=%d otg=%08x soft=%u\n"
		"setups=%u in_packets=%u out_packets=%u resets=%u errors=%u\n",
		usb->vbus, !!(rm_read(usb, RM_USB_SYSCON1) & RM_USB_PULLUP),
		usb->gadget.speed, rm_read(usb, RM_USB_DEVSTAT),
		rm_read(usb, RM_USB_IRQ_SRC), usb->fault,
		betty_control, error, readl(usb->otg + RM_OTG_CTRL), usb->softconnect,
		usb->setups, usb->in_packets, usb->out_packets, usb->resets, usb->errors);
	spin_unlock_irqrestore(&usb->lock, flags);
	return length;
}
static DEVICE_ATTR_RO(status);

/* Thirteen short rows fit the 30-column LCD, including PID1's "init: "
 * prefix. Keep this separate from the full shell-readable status report. */
static ssize_t enumeration_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rm_usb *usb = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t length;
	u16 betty = 0;
	int error = rm443_cbus_read(RM_USB_BETTY, RM_USB_BETTY_USBR, &betty);
	spin_lock_irqsave(&usb->lock, flags);
	length = sysfs_emit(buf,
		"USB enumeration\n"
		"VB=%u PU=%u F=%u\n"
		"DS=%04x IE=%04x\n"
		"IRQ=%04x N=%u\n"
		"SET=%u RST=%u\n"
		"TX=%u ACK=%u\n"
		"RX=%u ERR=%u\n"
		"EP0=%u busy=%u\n"
		"RQ=%02x/%02x V=%04x\n"
		"L=%u I=%04x\n"
		"B6=%04x CB=%d\n"
		"SOF=%04x SC=%04x\n"
		"OTG=%08x\n",
		usb->vbus, !!(rm_read(usb, RM_USB_SYSCON1) & RM_USB_PULLUP), usb->fault,
		rm_read(usb, RM_USB_DEVSTAT), rm_read(usb, RM_USB_IRQ_EN),
		rm_read(usb, RM_USB_IRQ_SRC), usb->irq_calls,
		usb->setups, usb->resets, usb->in_packets, usb->in_acks,
		usb->out_packets, usb->errors, usb->ep0_state, usb->eps[0].busy,
		usb->last_setup.bRequestType, usb->last_setup.bRequest,
		le16_to_cpu(usb->last_setup.wValue),
		le16_to_cpu(usb->last_setup.wLength), le16_to_cpu(usb->last_setup.wIndex),
		betty, error, rm_read(usb, RM_USB_SOF), rm_read(usb, RM_USB_SYSCON1),
		readl(usb->otg + RM_OTG_CTRL));
	spin_unlock_irqrestore(&usb->lock, flags);
	return length;
}
static DEVICE_ATTR_RO(enumeration);
static struct attribute *rm_usb_attrs[] = {
	&dev_attr_status.attr, &dev_attr_enumeration.attr, NULL
};
static const struct attribute_group rm_usb_group = { .attrs = rm_usb_attrs };

static void rm_wait_for_console(struct rm_usb *usb)
{
	unsigned int wait_ms = min_t(unsigned int, boot_wait_ms, 30000);
	unsigned long deadline;

	if (!wait_ms)
		return;
	deadline = jiffies + msecs_to_jiffies(wait_ms);
	dev_info(usb->dev, "waiting up to %u ms for host ACM DTR\n", wait_ms);
	/* No UDC lock is held: IRQs and VBUS work must enumerate the gadget
	 * while init sleeps. Keep both ordinary hardware watchdogs serviced;
	 * their registered worker may not have started this early in boot. */
	while (!READ_ONCE(usb->console_open) && time_before(jiffies, deadline)) {
		int error = rm443_power_refresh();

		if (error)
			dev_err_once(usb->dev, "watchdog refresh failed during USB boot wait: %d\n", error);
		msleep(20);
	}
	dev_info(usb->dev, "host ACM DTR %s; continuing boot\n",
		 READ_ONCE(usb->console_open) ? "attached" : "wait timed out");
}

static int rm_usb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rm_usb *usb;
	int error, irq, i;
	usb = devm_kzalloc(dev, sizeof(*usb), GFP_KERNEL);
	if (!usb)
		return -ENOMEM;
	usb->dev = dev;
	usb->regs = devm_platform_ioremap_resource_byname(pdev, "udc");
	usb->otg = devm_platform_ioremap_resource_byname(pdev, "otg");
	usb->clocks = devm_platform_ioremap_resource_byname(pdev, "clocks");
	usb->semaphore = devm_platform_ioremap_resource_byname(pdev, "semaphore");
	usb->pinmux = devm_platform_ioremap_resource_byname(pdev, "pinmux");
	if (IS_ERR(usb->regs))
		return PTR_ERR(usb->regs);
	if (IS_ERR(usb->otg))
		return PTR_ERR(usb->otg);
	if (IS_ERR(usb->clocks))
		return PTR_ERR(usb->clocks);
	if (IS_ERR(usb->semaphore))
		return PTR_ERR(usb->semaphore);
	if (IS_ERR(usb->pinmux))
		return PTR_ERR(usb->pinmux);
	spin_lock_init(&usb->lock);
	INIT_DELAYED_WORK(&usb->vbus_work, rm_vbus_poll);
	platform_set_drvdata(pdev, usb);
	error = rm_usb_hardware_init(usb);
	if (error)
		return dev_err_probe(dev, error, "USB clock/PHY/reset initialization failed\n");
	usb->gadget.ops = &rm_gadget_ops;
	usb->gadget.name = "rm443-udc";
	usb->gadget.max_speed = USB_SPEED_FULL;
	usb->gadget.speed = USB_SPEED_UNKNOWN;
	usb->gadget.is_selfpowered = true;
	rm_init_endpoints(usb);
	for (i = 0; i < 2; ++i) {
		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			return irq;
		error = devm_request_irq(dev, irq, rm_usb_irq, 0, dev_name(dev), usb);
		if (error)
			return error;
	}
	error = devm_device_add_group(dev, &rm_usb_group);
	if (error)
		return error;
	error = usb_add_gadget_udc(dev, &usb->gadget);
	if (error)
		return error;
	schedule_delayed_work(&usb->vbus_work, 0);
	dev_info(dev, "RAPS USB controller ready, Betty transceiver (full-speed PIO)\n");
	rm_wait_for_console(usb);
	return 0;
}

static void rm_usb_remove(struct platform_device *pdev)
{
	struct rm_usb *usb = platform_get_drvdata(pdev);
	WRITE_ONCE(usb->removing, true);
	cancel_delayed_work_sync(&usb->vbus_work);
	usb_del_gadget_udc(&usb->gadget);
	rm_write(usb, RM_USB_IRQ_EN, 0);
	/* Shared clock gates remain enabled: their other owners are unknown. */
}

static const struct of_device_id rm_usb_match[] = {
	{ .compatible = "nokia,rm443-usb" }, { }
};
MODULE_DEVICE_TABLE(of, rm_usb_match);
static struct platform_driver rm_usb_driver = {
	.probe = rm_usb_probe, .remove = rm_usb_remove,
	.driver = { .name = "rm443-usb", .of_match_table = rm_usb_match },
};
module_platform_driver(rm_usb_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Nokia RM-443 USB PIO device controller");
