/*
 * usb_bridge.h - USB composite device (dual CDC ACM + CMSIS-DAP v2) internals.
 *
 * Device identity: VID/PID 1209:0010 (pid.codes test PID), five interfaces:
 * two IAD-grouped CDC ACM functions bound by cdc_acm (Linux) / usbser
 * (Windows 10+) as serial ports for channel A (uart6, PC6/PC7) and channel B
 * (uart1, PA9/PA10), plus one vendor interface running CMSIS-DAP v2 bulk
 * (WinUSB auto-bind via BOS + MS OS 2.0 descriptors).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USB_BRIDGE_H
#define USB_BRIDGE_H

#include <rtthread.h>
#include <stdint.h>

/* Single OTG_FS bus */
#define USB_BRIDGE_BUSID       0
#define USB_BRIDGE_OTG_FS_BASE 0x50000000UL

/* Endpoint map: F446 OTG_FS offers EP0 + EP1..EP5 IN / EP1..EP3 OUT usage.
 * EP1/EP2 = CDC data bulk pairs, EP3 = DAP bulk pair, EP4/EP5 = CDC notify
 * interrupt IN (idle, required by the ACM comm interface). */
#define USB_BRIDGE_A_IN_EP      0x81U
#define USB_BRIDGE_A_OUT_EP     0x01U
#define USB_BRIDGE_B_IN_EP      0x82U
#define USB_BRIDGE_B_OUT_EP     0x02U
#define USB_BRIDGE_DAP_OUT_EP   0x03U
#define USB_BRIDGE_DAP_IN_EP    0x83U
#define USB_BRIDGE_A_NOTIFY_EP  0x84U
#define USB_BRIDGE_B_NOTIFY_EP  0x85U

#define USB_BRIDGE_BULK_MPS 64U

/* Interface layout (config descriptor order, see bridge_desc.c) */
#define USB_BRIDGE_INTF_CDC0_COMM 0U
#define USB_BRIDGE_INTF_CDC0_DATA 1U
#define USB_BRIDGE_INTF_CDC1_COMM 2U
#define USB_BRIDGE_INTF_CDC1_DATA 3U
#define USB_BRIDGE_INTF_DAP       4U

/* Bridge channel index */
#define USB_BRIDGE_CH_A 0U
#define USB_BRIDGE_CH_B 1U
#define USB_BRIDGE_CH_NUM 2U

/* Serial line format requested by the host via CDC SET_LINE_CODING */
struct cdc_line_state {
    uint32_t baud;      /* dwDTERate, 0 = not set yet */
    uint8_t data_bits;  /* bDataBits: 7 or 8 */
    uint8_t parity;     /* bParityType: 0 none / 1 odd / 2 even / 3 mark / 4 space */
    uint8_t stop_bits;  /* bCharFormat: 0 = 1 stop, 1 = 1.5, 2 = 2 */
    uint8_t dtr;
    uint8_t rts;
};

/* Per-channel line state owned by cdc_proto.c; pumps consume it. */
struct cdc_channel_state {
    struct cdc_line_state line;
    volatile uint32_t line_seq; /* bumped on every line change; pump re-applies */
};

extern struct cdc_channel_state g_cdc_ch[USB_BRIDGE_CH_NUM];

/* Data pumps (bridge_pump.c) */
void bridge_pump_register_endpoints(void);
void bridge_pump_notify_event(uint8_t event);
rt_err_t bridge_pump_start(void);
void bridge_pump_stat(void);
void bridge_pump_dump_ep(void);

/* GPIO vendor requests (EP0), same code points as ailink-f407 */
#define USB_BRIDGE_GPIO_REQ_CONFIG 0x60U /* wValue = dir<<8 | mask */
#define USB_BRIDGE_GPIO_REQ_WRITE  0x61U /* wValue = level<<8 | mask */
#define USB_BRIDGE_GPIO_REQ_READ   0x62U /* IN 2B: [levels, dir_mask] */

/* GPIO bank handler (bridge_gpio.c), runs in USB ISR */
int bridge_gpio_request_handler(struct usb_setup_packet *setup,
                                uint8_t **data, uint32_t *len);

/*
 * Generic EP0 vendor extension point: fills the second vendor slot so
 * usbd_core's dispatch chain reaches the extension after the GPIO handler
 * declines a request. The handler runs in USB ISR context and must return
 * 0 to claim a request, -1 to pass. One slot, first come only (currently
 * the OTA module); registering before or after usbd_initialize both work
 * since the chain reads the pointer per request.
 */
typedef int (*usb_bridge_vendor_ext_t)(uint8_t busid,
                                       struct usb_setup_packet *setup,
                                       uint8_t **data, uint32_t *len);
rt_err_t usb_bridge_register_vendor_ext(usb_bridge_vendor_ext_t handler);

/* EP0 vendor dispatcher (cdc_proto.c): GPIO bank + extension slot */
int bridge_vendor_request_handler(uint8_t busid, struct usb_setup_packet *setup,
                                  uint8_t **data, uint32_t *len);

/* CMSIS-DAP v2 transport (bridge_dap.c) */
void bridge_dap_register_endpoints(void);
void bridge_dap_notify_event(uint8_t event);
rt_err_t bridge_dap_start(void);

#endif /* USB_BRIDGE_H */
