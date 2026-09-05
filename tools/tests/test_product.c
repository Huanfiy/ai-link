/* Product state transitions and register preservation.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ailink_product.h"
#include "usb_bridge.h"
#include <assert.h>
#include <board.h>
#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include <usbd_core.h>

GPIO_TypeDef test_gpio[3];
RCC_TypeDef test_rcc;
static rt_tick_t tick;
static bool configured, suspended;
static rt_base_t irq_level;
static struct rt_timer *timer;
static rt_err_t start_result;

rt_base_t rt_hw_interrupt_disable(void)
{
    return irq_level++;
}
void rt_hw_interrupt_enable(rt_base_t level)
{
    assert(irq_level == level + 1);
    irq_level = level;
}
rt_tick_t rt_tick_get(void)
{
    return tick;
}
bool usb_device_is_configured(uint8_t busid)
{
    assert(busid == 0);
    return configured;
}
bool usb_device_is_suspend(uint8_t busid)
{
    assert(busid == 0);
    return suspended;
}
void rt_timer_init(struct rt_timer *t, const char *name, void (*cb)(void *), void *parameter,
                   rt_tick_t period, unsigned flags)
{
    (void)name;
    assert(period == rt_tick_from_millisecond(10));
    assert(flags == RT_TIMER_FLAG_PERIODIC);
    t->callback = cb;
    t->parameter = parameter;
}
rt_err_t rt_timer_start(struct rt_timer *t)
{
    timer = t;
    return start_result;
}
static void poll_ms(unsigned ms)
{
    tick += rt_tick_from_millisecond(ms);
    timer->callback(timer->parameter);
    assert(irq_level == 0);
}
static uint8_t lamps(void)
{
    const unsigned pins[] = {3, 4, 5, 8};
    uint8_t result = 0;
    /* BSRR is write-only on silicon: check the emitted atomic command. */
    assert(!(GPIOC->BSRR & ((1U << 9) | (1UL << 25))));
    for (unsigned i = 0; i < 4; i++)
        if (GPIOC->BSRR & (1UL << (pins[i] + 16)))
            result |= 1U << i;
    return result;
}

int main(void)
{
    memset(test_gpio, 0xA5, sizeof(test_gpio));
    const uint32_t pa_usb_swd = (3UL << 22) | (3UL << 24) | (3UL << 26) | (3UL << 28);
    uint32_t preserved = GPIOA->MODER & pa_usb_swd;
    ailink_product_hw_init();
    assert(!ailink_target_is_ready());
    assert((GPIOA->MODER & pa_usb_swd) == preserved);
    assert(GPIOC->BSRR == (1UL << 25));
    start_result = -7;
    assert(bridge_product_start() == -7);
    start_result = 0;
    assert(bridge_product_start() == RT_EOK);
    poll_ms(10);
    assert(!ailink_target_is_ready() && lamps() == 0);

    configured = true;
    bridge_product_notify_event(USBD_EVENT_CONFIGURED);
    poll_ms(10);
    assert(!ailink_target_is_ready());
    if (TEST_TICK_HZ >= 1000)
    {
        poll_ms(3);
        assert(!ailink_target_is_ready());
        poll_ms(1);
    }
    else
        poll_ms(10);
    assert(ailink_target_is_ready());
    assert((GPIOA->MODER & pa_usb_swd) == preserved);

    bridge_activity_rx(0, 0);
    bridge_activity_tx_start(255);
    poll_ms(10);
    assert(lamps() == 0);
    bridge_activity_tx_start(0);
    bridge_activity_rx(1, 1);
    poll_ms(10);
    assert(lamps() == 9);
    poll_ms(50);
    assert(lamps() == 1); /* TX stays on throughout a long DMA transfer. */
    bridge_activity_tx_done(0);
    poll_ms(40);
    assert(lamps() == 1);
    poll_ms(10);
    assert(lamps() == 0);
    tick = UINT32_MAX - rt_tick_from_millisecond(20);
    bridge_activity_rx(0, 1);
    poll_ms(40);
    assert(lamps() == 2);
    poll_ms(10);
    assert(lamps() == 0);

    GPIOA->MODER = (GPIOA->MODER & ~3U) | 1U; /* Host sets G2 to output. */
    GPIOA->PUPDR = (GPIOA->PUPDR & ~3U) | 1U;
    suspended = true;
    bridge_product_notify_event(USBD_EVENT_SUSPEND);
    assert(!ailink_target_is_ready());
    assert((GPIOA->MODER & 3U) == 0 && (GPIOA->PUPDR & 3U) == 0);
    assert((GPIOA->MODER & pa_usb_swd) == preserved);
    bridge_activity_tx_done(0); /* Stale callbacks cannot relight a lamp. */
    bridge_activity_rx(1, 8);
    poll_ms(10);
    assert(lamps() == 0);
    suspended = false;
    bridge_product_notify_event(USBD_EVENT_RESUME);
    poll_ms(10);
    assert(!ailink_target_is_ready());
    poll_ms(10);
    assert(ailink_target_is_ready());
    assert((GPIOA->MODER & 3U) == 1 && (GPIOA->PUPDR & 3U) == 1);

    configured = false; /* SET_CONFIGURATION(0) has no stack event. */
    poll_ms(10);
    assert(!ailink_target_is_ready() && lamps() == 0);
    configured = true;
    poll_ms(10);
    poll_ms(10);
    assert(ailink_target_is_ready());
    bridge_product_notify_event(USBD_EVENT_RESET);
    configured = false;
    assert(!ailink_target_is_ready());
    configured = true;
    poll_ms(10);
    poll_ms(10);
    bridge_product_prepare_reboot();
    poll_ms(100);
    assert(!ailink_target_is_ready() && lamps() == 0);
    printf("product transitions / LEDs / register masks: PASS (%u Hz)\n", TEST_TICK_HZ);
    return 0;
}
