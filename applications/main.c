/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-11-06     SummerGift   first version
 * 2023-12-03     Meco Man     support nano version
 */

#include <board.h>
#include <rtthread.h>
#include <drv_gpio.h>
#ifndef RT_USING_NANO
#include <rtdevice.h>
#endif /* RT_USING_NANO */

/* Onboard RGB LEDs are active-low: LED1=PC0, LED2=PC1, LED3=PC2 ->
 * drive LOW to light. PC0 doubles as the bootloader fault indicator. */
#define LED1_PIN    GET_PIN(C, 0)
#define LED2_PIN    GET_PIN(C, 1)
#define LED3_PIN    GET_PIN(C, 2)

#define LED_MARQUEE_INTERVAL    150 /* ms per step */

int main(void)
{
    const rt_base_t leds[] = {LED1_PIN, LED2_PIN, LED3_PIN};
    const rt_size_t led_num = sizeof(leds) / sizeof(leds[0]);
    rt_size_t i;

    /* all LEDs start off (active-low: HIGH = off) */
    for (i = 0; i < led_num; i++)
    {
        rt_pin_mode(leds[i], PIN_MODE_OUTPUT);
        rt_pin_write(leds[i], PIN_HIGH);
    }

    i = 0;
    while (1)
    {
        rt_pin_write(leds[i], PIN_LOW);                 /* light current LED */
        rt_thread_mdelay(LED_MARQUEE_INTERVAL);
        rt_pin_write(leds[i], PIN_HIGH);                /* then turn it off */
        i = (i + 1) % led_num;                          /* advance to next */
    }
}
