/*
 * usb_config.h - CherryUSB stack configuration for the onboard OTG_FS device.
 *
 * Trimmed from $RTT_ROOT/components/drivers/usb/cherryusb/cherryusb_config_template.h
 * (device mode only, STM32F446 OTG_FS: FS PHY, slave mode, no data cache).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CHERRYUSB_CONFIG_H
#define CHERRYUSB_CONFIG_H

#include <rtthread.h>

/* ================ USB common Configuration ================ */

#define CONFIG_USB_PRINTF(...) rt_kprintf(__VA_ARGS__)

#ifndef CONFIG_USB_DBG_LEVEL
#define CONFIG_USB_DBG_LEVEL USB_DBG_INFO
#endif

/* Enable print with color */
#define CONFIG_USB_PRINTF_COLOR_ENABLE

/* No data cache on STM32F446, and OTG_FS runs in slave (FIFO) mode. */
#define CONFIG_USB_ALIGN_SIZE 4

/* No cache: plain RAM placement, no dedicated no-cache section needed. */
#define USB_NOCACHE_RAM_SECTION

/* ================= USB Device Stack Configuration ================ */

/* Ep0 in and out transfer buffer */
#ifndef CONFIG_USBDEV_REQUEST_BUFFER_LEN
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512
#endif

/* enable advance desc register api */
#define CONFIG_USBDEV_ADVANCE_DESC

#ifndef CONFIG_USBDEV_EP0_PRIO
#define CONFIG_USBDEV_EP0_PRIO 4
#endif

#ifndef CONFIG_USBDEV_EP0_STACKSIZE
#define CONFIG_USBDEV_EP0_STACKSIZE 2048
#endif

/* ================ USB HOST Stack Configuration ==================
 * Host mode is not used; these sizing macros are still required because the
 * dwc2 ST glue unconditionally includes usbh_core.h.
 */

#define CONFIG_USBHOST_MAX_RHPORTS          1
#define CONFIG_USBHOST_MAX_EXTHUBS          1
#define CONFIG_USBHOST_MAX_EHPORTS          4
#define CONFIG_USBHOST_MAX_INTERFACES       8
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS 2
#define CONFIG_USBHOST_MAX_ENDPOINTS        4
#define CONFIG_USBHOST_DEV_NAMELEN          16

#ifndef CONFIG_USBHOST_PSC_PRIO
#define CONFIG_USBHOST_PSC_PRIO 0
#endif
#ifndef CONFIG_USBHOST_PSC_STACKSIZE
#define CONFIG_USBHOST_PSC_STACKSIZE 2048
#endif

#ifndef CONFIG_USBHOST_REQUEST_BUFFER_LEN
#define CONFIG_USBHOST_REQUEST_BUFFER_LEN 512
#endif

#ifndef CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT
#define CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT 500
#endif

/* ================ USB Device Port Configuration ================ */

#ifndef CONFIG_USBDEV_MAX_BUS
#define CONFIG_USBDEV_MAX_BUS 1
#endif

/* ---------------- DWC2 Configuration ----------------
 * OTG_FS core: 320 words (1.25KB) total FIFO RAM, slave mode only.
 * Custom partition (words): RX shared 128 / EP0 TX 16 / EP1 TX 48 /
 * EP2 TX 48 / EP3 TX 48 / EP4 TX 16 / EP5 TX 16 -- see
 * board/ports/usbd_fs_port.c.
 */
#define CONFIG_USB_DWC2_CUSTOM_FIFO

#ifndef usb_phyaddr2ramaddr
#define usb_phyaddr2ramaddr(addr) (addr)
#endif

#ifndef usb_ramaddr2phyaddr
#define usb_ramaddr2phyaddr(addr) (addr)
#endif

#endif /* CHERRYUSB_CONFIG_H */
