/* SPDX-License-Identifier: GPL-2.0-only */
/* RM-443 USB device bank: firmware PIO paths agree with the OMAP UDC 3.2
 * register definitions in Linux drivers/usb/gadget/udc/omap_udc.h. */
#ifndef RM443_USB_H
#define RM443_USB_H

#define RM_USB_REV              0x00
#define RM_USB_EP_NUM           0x04
#define RM_USB_SETUP_SEL        BIT(6)
#define RM_USB_EP_SEL           BIT(5)
#define RM_USB_EP_DIR           BIT(4)
#define RM_USB_DATA             0x08
#define RM_USB_CTRL             0x0c
#define RM_USB_CLR_HALT         BIT(7)
#define RM_USB_SET_HALT         BIT(6)
#define RM_USB_CLR_TOGGLE       BIT(3)
#define RM_USB_FIFO_EN          BIT(2)
#define RM_USB_CLR_EP           BIT(1)
#define RM_USB_STAT             0x10
#define RM_USB_ACK              BIT(3)
#define RM_USB_RX_COUNT         0x14
#define RM_USB_SYSCON1          0x18
#define RM_USB_CFG_LOCK         BIT(8)
#define RM_USB_SELF_POWERED     BIT(2)
#define RM_USB_PULLUP           BIT(0)
#define RM_USB_SYSCON2          0x1c
#define RM_USB_STALL            BIT(5)
#define RM_USB_CONFIGURED       BIT(3)
#define RM_USB_CLEAR_CONFIG     BIT(2)
#define RM_USB_DEVSTAT          0x20
#define RM_USB_RESET            BIT(5)
#define RM_USB_SUSPEND          BIT(4)
#define RM_USB_ATTACHED         BIT(0)
#define RM_USB_SOF              0x24
#define RM_USB_IRQ_EN           0x28
#define RM_USB_DMA_IRQ_EN       0x2c
#define RM_USB_IRQ_SRC          0x30
#define RM_USB_IRQ_RX           BIT(5)
#define RM_USB_IRQ_TX           BIT(4)
#define RM_USB_IRQ_STATE        BIT(3)
#define RM_USB_IRQ_SETUP        BIT(2)
#define RM_USB_IRQ_EP0_RX       BIT(1)
#define RM_USB_IRQ_EP0_TX       BIT(0)
#define RM_USB_IRQ_MASK         0x7bf
#define RM_USB_IRQ_ENABLE       0x39
#define RM_USB_EPN_STAT         0x34
#define RM_USB_RX_DMA           0x40
#define RM_USB_TX_DMA           0x44
/* Firmware-specific transceiver/controller extension, not standard OMAP. */
#define RM_USB_CONNECT          0x78
#define RM_USB_EP_RX(n)         (0x80 + 4 * (n))
#define RM_USB_EP_TX(n)         (0xc0 + 4 * (n))
#define RM_USB_EP_VALID         BIT(15)
#define RM_OTG_SYSCON1          0x04
#define RM_OTG_SYSCON2          0x08
#define RM_OTG_CTRL             0x0c
#define RM_OTG_RESET            BIT(1)
#define RM_OTG_RESET_DONE       BIT(2)
#define RM_OTG_BSESSVLD         BIT(18)

#endif
