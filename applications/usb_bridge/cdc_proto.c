/*
 * cdc_proto.c - CDC ACM control plane + EP0 vendor request dispatch.
 *
 * The CherryUSB CDC ACM class handler decodes SET/GET_LINE_CODING and
 * SET_CONTROL_LINE_STATE and calls the usbd_cdc_acm_* hooks below (USB ISR
 * context): they only record state, the data pumps apply the UART
 * reconfiguration from thread context (HAL_UART_Init is not ISR-safe).
 *
 * Device-scoped EP0 vendor requests keep the ailink-f407 code points:
 * GPIO bank 0x60-0x62 (bridge_gpio.c) and the extension slot (OTA 0x63/0x64).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbd_core.h"
#include "usbd_cdc_acm.h"
#include "usb_cdc.h"

#include "usb_bridge.h"

struct cdc_channel_state g_cdc_ch[USB_BRIDGE_CH_NUM] = {
    { .line = { .baud = 115200, .data_bits = 8 } },
    { .line = { .baud = 115200, .data_bits = 8 } },
};

/* comm interface number (SET_LINE_CODING wIndex) -> bridge channel */
static struct cdc_channel_state *cdc_channel_from_intf(uint8_t intf)
{
    switch (intf) {
    case USB_BRIDGE_INTF_CDC0_COMM:
    case USB_BRIDGE_INTF_CDC0_DATA:
        return &g_cdc_ch[USB_BRIDGE_CH_A];
    case USB_BRIDGE_INTF_CDC1_COMM:
    case USB_BRIDGE_INTF_CDC1_DATA:
        return &g_cdc_ch[USB_BRIDGE_CH_B];
    default:
        return RT_NULL;
    }
}

void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf,
                                  struct cdc_line_coding *line_coding)
{
    struct cdc_channel_state *st = cdc_channel_from_intf(intf);

    (void)busid;
    if (st == RT_NULL) {
        return;
    }
    st->line.baud = line_coding->dwDTERate;
    st->line.data_bits = line_coding->bDataBits;
    st->line.parity = line_coding->bParityType;
    st->line.stop_bits = line_coding->bCharFormat;
    st->line_seq++;
}

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf,
                                  struct cdc_line_coding *line_coding)
{
    struct cdc_channel_state *st = cdc_channel_from_intf(intf);

    (void)busid;
    if (st == RT_NULL) {
        return;
    }
    line_coding->dwDTERate = st->line.baud;
    line_coding->bDataBits = st->line.data_bits;
    line_coding->bParityType = st->line.parity;
    line_coding->bCharFormat = st->line.stop_bits;
}

void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    struct cdc_channel_state *st = cdc_channel_from_intf(intf);

    (void)busid;
    if (st != RT_NULL) {
        st->line.dtr = dtr ? 1 : 0;
    }
}

void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t intf, bool rts)
{
    struct cdc_channel_state *st = cdc_channel_from_intf(intf);

    (void)busid;
    if (st != RT_NULL) {
        st->line.rts = rts ? 1 : 0;
    }
}

/* ---------------- EP0 vendor dispatch (GPIO + extension slot) ------------- */

static usb_bridge_vendor_ext_t vendor_ext_handler;

rt_err_t usb_bridge_register_vendor_ext(usb_bridge_vendor_ext_t handler)
{
    if (handler == RT_NULL || vendor_ext_handler != RT_NULL) {
        return -RT_ERROR;
    }
    vendor_ext_handler = handler;
    return RT_EOK;
}

int bridge_vendor_request_handler(uint8_t busid, struct usb_setup_packet *setup,
                                  uint8_t **data, uint32_t *len)
{
    if (setup->bRequest >= USB_BRIDGE_GPIO_REQ_CONFIG &&
        setup->bRequest <= USB_BRIDGE_GPIO_REQ_READ) {
        return bridge_gpio_request_handler(setup, data, len);
    }

    if (vendor_ext_handler != RT_NULL) {
        return vendor_ext_handler(busid, setup, data, len);
    }
    return -1;
}

/* ---------------- diagnostics ---------------- */

static void usbbr_stat(void)
{
    static const char *const parity_name[] = { "N", "O", "E", "M", "S" };
    static const char *const stop_name[] = { "1", "1.5", "2" };

    for (uint8_t i = 0; i < USB_BRIDGE_CH_NUM; i++) {
        struct cdc_channel_state *st = &g_cdc_ch[i];
        uint8_t parity = (st->line.parity < 5) ? st->line.parity : 0;
        uint8_t stop = (st->line.stop_bits < 3) ? st->line.stop_bits : 0;

        rt_kprintf("ch%c: baud=%u %u%s%s dtr=%u rts=%u seq=%u\n",
                   'A' + i, st->line.baud, st->line.data_bits,
                   parity_name[parity], stop_name[stop],
                   st->line.dtr, st->line.rts, st->line_seq);
    }
    bridge_pump_stat();
}
MSH_CMD_EXPORT(usbbr_stat, usb bridge channel status);

static void usbbr_ep(void)
{
    bridge_pump_dump_ep();
}
MSH_CMD_EXPORT(usbbr_ep, usb bridge ep regs);
