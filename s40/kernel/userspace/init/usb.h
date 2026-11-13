/* SPDX-License-Identifier: MIT */
#ifndef INIT_USB_H
#define INIT_USB_H

void usb_status(void);
void usb_child_exited(pid_t child);
void usb_service(void);

#endif
