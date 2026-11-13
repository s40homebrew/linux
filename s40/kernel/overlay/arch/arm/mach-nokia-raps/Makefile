# SPDX-License-Identifier: GPL-2.0-only
obj-y += board.o ramcon.o irq.o timer.o rm443_keys.o
obj-y += rm443_power.o rm443_power_key.o
# The pre-registered CDC gadget binds here before secure service/MMC/DRM.
obj-$(CONFIG_USB_GADGET) += rm443_usb.o
obj-y += rm443_secure_wdt.o rm443_secure_wdt_call.o
obj-y += rm443_boot_stage.o
obj-$(CONFIG_RM443_INSPECT) += rm443_inspect.o rm443_inspect_cpu.o
obj-$(CONFIG_RM443_PROBE) += rm443_probe.o rm443_probe_load.o
obj-$(CONFIG_MMC) += rm443_mmc.o
