/* Product host-test RTOS boundary. SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef uint32_t rt_tick_t;
typedef size_t rt_size_t;
typedef int rt_base_t;
typedef int rt_err_t;
#define RT_NULL                NULL
#define RT_EOK                 0
#define RT_TIMER_FLAG_PERIODIC 1
struct rt_timer
{
    void (*callback)(void *);
    void *parameter;
};
rt_base_t rt_hw_interrupt_disable(void);
void rt_hw_interrupt_enable(rt_base_t level);
rt_tick_t rt_tick_get(void);
static inline rt_tick_t rt_tick_from_millisecond(int ms)
{
    return (ms * TEST_TICK_HZ + 999U) / 1000U;
}
void rt_timer_init(struct rt_timer *, const char *, void (*)(void *), void *, rt_tick_t, unsigned);
rt_err_t rt_timer_start(struct rt_timer *);
