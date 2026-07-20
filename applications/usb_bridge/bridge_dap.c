/*
 * bridge_dap.c - CMSIS-DAP v2 (bulk) transport thread on EP3.
 *
 * Command/response loop: OUT packets are queued into a small ring by the
 * OUT-complete ISR (re-armed immediately unless the ring is full, in which
 * case the host sees NAK), the DAP thread executes commands with the ARM
 * reference DAP_ExecuteCommand and streams responses back over IN.
 * SWD bit-banging lives in board/ports/DAP_config.h (PA4/PA5/PA6).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtdevice.h>

#include "usbd_core.h"

#include "DAP_config.h"
#include "DAP.h"

#include "usb_bridge.h"

#define DAP_EV_OUT (1U << 0)
#define DAP_EV_IN  (1U << 1)

/* DAP_Info identification strings queried via ID_DAP_Info (reference DAP.c
 * calls these; a 0 return means "not available"). */
uint8_t DAP_GetVendorString(char *str)
{
    return (uint8_t)(rt_strlen(rt_strncpy(str, "ailink", 60)) + 1U);
}

uint8_t DAP_GetProductString(char *str)
{
    return (uint8_t)(rt_strlen(rt_strncpy(str, "ailink CMSIS-DAP v2", 60)) + 1U);
}

uint8_t DAP_GetSerNumString(char *str)
{
    (void)str;
    return 0U; /* device iSerialNumber already carries the UID */
}

uint8_t DAP_GetTargetDeviceVendorString(char *str)
{
    (void)str;
    return 0U;
}

uint8_t DAP_GetTargetDeviceNameString(char *str)
{
    (void)str;
    return 0U;
}

uint8_t DAP_GetTargetBoardVendorString(char *str)
{
    (void)str;
    return 0U;
}

uint8_t DAP_GetTargetBoardNameString(char *str)
{
    (void)str;
    return 0U;
}

uint8_t DAP_GetProductFirmwareVersionString(char *str)
{
    (void)str;
    return 0U;
}

/* request/response rings sized by the reference DAP_PACKET_COUNT (4) */
static uint8_t req_buf[DAP_PACKET_COUNT][DAP_PACKET_SIZE] __attribute__((aligned(4)));
static uint16_t req_len[DAP_PACKET_COUNT];
static volatile uint8_t req_head; /* ISR writes */
static volatile uint8_t req_tail; /* thread reads */
static volatile uint8_t req_armed;

static uint8_t resp_buf[DAP_PACKET_SIZE] __attribute__((aligned(4)));
static volatile uint8_t in_busy;
static volatile uint8_t configured;

static struct rt_event dap_ev;
static rt_uint8_t dap_stack[2048];
static struct rt_thread dap_thread;

static uint8_t req_count(void)
{
    return (uint8_t)((req_head - req_tail) & 0xFF) % (2 * DAP_PACKET_COUNT);
}

/* ---------------- USB ISR side ---------------- */

static void dap_try_arm_out(void)
{
    if (!configured || req_armed) {
        return;
    }
    if (((req_head - req_tail) & 0xFF) >= DAP_PACKET_COUNT) {
        return; /* ring full: NAK until the thread consumes a slot */
    }
    req_armed = 1;
    usbd_ep_start_read(USB_BRIDGE_BUSID, USB_BRIDGE_DAP_OUT_EP,
                       req_buf[req_head % DAP_PACKET_COUNT], DAP_PACKET_SIZE);
}

static void dap_usb_out_complete(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;

    req_armed = 0;
    if (nbytes > 0) {
        req_len[req_head % DAP_PACKET_COUNT] = (uint16_t)nbytes;
        req_head++;
        rt_event_send(&dap_ev, DAP_EV_OUT);
    }
    dap_try_arm_out();
}

static void dap_usb_in_complete(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
    in_busy = 0;
    rt_event_send(&dap_ev, DAP_EV_IN);
}

/* ---------------- thread ---------------- */

static void dap_entry(void *param)
{
    rt_uint32_t ev;

    (void)param;

    while (1) {
        rt_event_recv(&dap_ev, DAP_EV_OUT | DAP_EV_IN,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &ev);

        while (req_tail != req_head) {
            if (in_busy) {
                break; /* previous response still in flight */
            }

            uint8_t idx = req_tail % DAP_PACKET_COUNT;
            uint32_t resp = DAP_ExecuteCommand(req_buf[idx], resp_buf);
            uint32_t resp_len = resp & 0xFFFFU;

            req_tail++;
            {
                rt_base_t level = rt_hw_interrupt_disable();
                dap_try_arm_out(); /* slot freed: resume OUT if it was full */
                in_busy = 1;
                usbd_ep_start_write(USB_BRIDGE_BUSID, USB_BRIDGE_DAP_IN_EP,
                                    resp_buf, resp_len);
                rt_hw_interrupt_enable(level);
            }
        }
    }
}

/* ---------------- wiring (called from bridge_desc.c) ---------------- */

static struct usbd_endpoint dap_out_ep = {
    .ep_addr = USB_BRIDGE_DAP_OUT_EP,
    .ep_cb = dap_usb_out_complete,
};
static struct usbd_endpoint dap_in_ep = {
    .ep_addr = USB_BRIDGE_DAP_IN_EP,
    .ep_cb = dap_usb_in_complete,
};

void bridge_dap_register_endpoints(void)
{
    usbd_add_endpoint(USB_BRIDGE_BUSID, &dap_out_ep);
    usbd_add_endpoint(USB_BRIDGE_BUSID, &dap_in_ep);
}

void bridge_dap_notify_event(uint8_t event)
{
    switch (event) {
    case USBD_EVENT_CONFIGURED:
        configured = 1;
        req_head = 0;
        req_tail = 0;
        req_armed = 0;
        in_busy = 0;
        dap_try_arm_out();
        break;
    case USBD_EVENT_RESET:
    case USBD_EVENT_DISCONNECTED:
        configured = 0;
        break;
    default:
        break;
    }
}

rt_err_t bridge_dap_start(void)
{
    rt_event_init(&dap_ev, "usbdap", RT_IPC_FLAG_FIFO);

    DAP_Setup();

    rt_thread_init(&dap_thread, "usbdap", dap_entry, RT_NULL,
                   dap_stack, sizeof(dap_stack), 9, 10);
    rt_thread_startup(&dap_thread);
    return RT_EOK;
}
