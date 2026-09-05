/*
 * bridge_product.c - Product supply policy and four independent UART lamps.
 *
 * The 10ms timer refreshes indicators without adding byte interrupts. It
 * also observes SET_CONFIGURATION(0), for which this CherryUSB revision
 * does not emit a configuration event. Reset/suspend/disconnect stop power
 * synchronously in the USB event callback.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ailink_product.h"
#include "usb_bridge.h"
#include "usbd_core.h"
#include <rtthread.h>

#define LED_HOLD_MS     50
#define PRODUCT_POLL_MS 10
#define POWER_SETTLE_MS 4 /* TPS2553 specified maximum turn-on is 3ms. */

struct activity_state
{
    rt_tick_t last;
    bool valid;
    bool inflight;
};

static struct activity_state activities[4];
static struct rt_timer product_timer;
static volatile bool power_requested;
static volatile bool rebooting;
static rt_tick_t power_start;

static void product_stop(void)
{
    ailink_target_power_stop();
    power_requested = false;
    for (uint32_t i = 0; i < 4; i++)
    {
        activities[i].valid = false;
        activities[i].inflight = false;
    }
    ailink_product_leds_write(0, false);
}

static bool product_configured(void)
{
    return !rebooting && usb_device_is_configured(USB_BRIDGE_BUSID) &&
           !usb_device_is_suspend(USB_BRIDGE_BUSID);
}

static void product_poll(void *parameter)
{
    (void)parameter;
    rt_base_t level = rt_hw_interrupt_disable();
    rt_tick_t now = rt_tick_get();
    uint8_t mask = 0;

    if (!product_configured())
    {
        product_stop();
        rt_hw_interrupt_enable(level);
        return;
    }
    if (!power_requested)
    {
        ailink_target_power_start();
        power_start = now;
        power_requested = true;
    }
    else if (!ailink_target_is_ready() &&
             (rt_tick_t)(now - power_start) >= rt_tick_from_millisecond(POWER_SETTLE_MS))
    {
        ailink_target_power_ready();
    }
    if (ailink_target_is_ready())
    {
        for (uint32_t i = 0; i < 4; i++)
        {
            if (activities[i].inflight ||
                (activities[i].valid &&
                 (rt_tick_t)(now - activities[i].last) < rt_tick_from_millisecond(LED_HOLD_MS)))
            {
                mask |= (uint8_t)(1U << i);
            }
            else
            {
                activities[i].valid = false;
            }
        }
    }
    ailink_product_leds_write(mask, true);
    rt_hw_interrupt_enable(level);
}

void bridge_activity_tx_start(uint8_t channel)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (channel < USB_BRIDGE_CH_NUM && ailink_target_is_ready() && product_configured())
    {
        activities[channel * 2U].inflight = true;
        activities[channel * 2U].valid = true;
        activities[channel * 2U].last = rt_tick_get();
    }
    rt_hw_interrupt_enable(level);
}

void bridge_activity_tx_done(uint8_t channel)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (channel < USB_BRIDGE_CH_NUM && activities[channel * 2U].inflight)
    {
        activities[channel * 2U].inflight = false;
        activities[channel * 2U].last = rt_tick_get();
    }
    rt_hw_interrupt_enable(level);
}

void bridge_activity_rx(uint8_t channel, rt_size_t size)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (channel < USB_BRIDGE_CH_NUM && size != 0 && ailink_target_is_ready() &&
        product_configured())
    {
        activities[channel * 2U + 1U].valid = true;
        activities[channel * 2U + 1U].last = rt_tick_get();
    }
    rt_hw_interrupt_enable(level);
}

void bridge_product_notify_event(uint8_t event)
{
    switch (event)
    {
        case USBD_EVENT_RESET:
        case USBD_EVENT_DISCONNECTED:
        case USBD_EVENT_SUSPEND:
        case USBD_EVENT_DEINIT: {
            rt_base_t level = rt_hw_interrupt_disable();
            product_stop();
            rt_hw_interrupt_enable(level);
            break;
        }
        default:
            break;
    }
}

void bridge_product_prepare_reboot(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rebooting = true;
    product_stop();
    rt_hw_interrupt_enable(level);
}

rt_err_t bridge_product_start(void)
{
    product_stop();
    rt_timer_init(&product_timer, "product", product_poll, RT_NULL,
                  rt_tick_from_millisecond(PRODUCT_POLL_MS), RT_TIMER_FLAG_PERIODIC);
    return rt_timer_start(&product_timer);
}
