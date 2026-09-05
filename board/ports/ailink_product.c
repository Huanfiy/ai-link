/*
 * ailink_product.c - Protected power, target pin gating and product indicators.
 *
 * All mask operations preserve the USB, console, SPI and self-SWD pins.
 * Target modes survive suspend; the serial alternate functions are enabled
 * only after the load switches have had time to turn on.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <board.h>
#include <rtthread.h>

#include "ailink_product.h"

#define PA_TARGET                                                                                  \
    ((1U << 0) | (1U << 1) | (1U << 4) | (1U << 5) | (1U << 6) | (1U << 8) | (1U << 9) | (1U << 10))
#define PB_TARGET     ((1U << 0) | (1U << 1) | (1U << 8) | (1U << 9) | (1U << 10))
#define PC_TARGET     ((1U << 6) | (1U << 7))
#define ACTIVITY_PINS ((1U << 3) | (1U << 4) | (1U << 5) | (1U << 8))
#define RGB_PINS      7U
#define POWER_PIN     (1U << 9)

struct target_port
{
    GPIO_TypeDef *port;
    uint16_t pins;
    uint32_t mode;
    uint32_t pull;
};

static struct target_port target_ports[] = {
    {GPIOA, PA_TARGET, 0, 0},
    {GPIOB, PB_TARGET, 0, 0},
    {GPIOC, PC_TARGET, 0, 0},
};
static volatile bool target_ready;

static uint32_t mode_mask(uint16_t pins)
{
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 16; i++)
    {
        if (pins & (1U << i))
        {
            mask |= 3UL << (i * 2U);
        }
    }
    return mask;
}

bool ailink_target_is_ready(void)
{
    return target_ready;
}

void ailink_target_power_stop(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool save = target_ready;

    target_ready = false;
    for (uint32_t i = 0; i < sizeof(target_ports) / sizeof(target_ports[0]); i++)
    {
        struct target_port *p = &target_ports[i];
        uint32_t mask = mode_mask(p->pins);
        if (save)
        {
            p->mode = p->port->MODER & mask;
            p->pull = p->port->PUPDR & mask;
        }
        p->port->MODER &= ~mask;
        p->port->PUPDR &= ~mask;
    }
    GPIOC->BSRR = POWER_PIN << 16U;
    rt_hw_interrupt_enable(level);
}

void ailink_target_power_start(void)
{
    GPIOC->BSRR = POWER_PIN;
}

void ailink_target_power_ready(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    for (uint32_t i = 0; i < sizeof(target_ports) / sizeof(target_ports[0]); i++)
    {
        struct target_port *p = &target_ports[i];
        uint32_t mask = mode_mask(p->pins);
        p->port->MODER = (p->port->MODER & ~mask) | p->mode;
        p->port->PUPDR = (p->port->PUPDR & ~mask) | p->pull;
    }
    /* HAL MSP deliberately leaves these inputs while power is absent. */
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((15UL << 4) | (15UL << 8))) | (7UL << 4) | (7UL << 8);
    GPIOC->AFR[0] = (GPIOC->AFR[0] & ~((15UL << 24) | (15UL << 28))) | (8UL << 24) | (8UL << 28);
    GPIOA->OSPEEDR |= (3UL << 18) | (3UL << 20);
    GPIOC->OSPEEDR |= (3UL << 12) | (3UL << 14);
    GPIOA->OTYPER &= ~((1UL << 9) | (1UL << 10));
    GPIOC->OTYPER &= ~PC_TARGET;
    GPIOA->PUPDR = (GPIOA->PUPDR & ~((3UL << 18) | (3UL << 20))) | (1UL << 18) | (1UL << 20);
    GPIOC->PUPDR = (GPIOC->PUPDR & ~((3UL << 12) | (3UL << 14))) | (1UL << 12) | (1UL << 14);
    GPIOA->MODER = (GPIOA->MODER & ~((3UL << 18) | (3UL << 20))) | (2UL << 18) | (2UL << 20);
    GPIOC->MODER = (GPIOC->MODER & ~((3UL << 12) | (3UL << 14))) | (2UL << 12) | (2UL << 14);
    target_ready = true;
    rt_hw_interrupt_enable(level);
}

void ailink_product_leds_write(uint8_t activity, bool configured)
{
    const uint8_t pins[4] = {3, 4, 5, 8};
    uint32_t on = configured ? (1UL << 1) : 0; /* RGB green, PC1 */
    for (uint32_t i = 0; i < 4; i++)
    {
        if (activity & (1U << i))
        {
            on |= 1UL << pins[i];
        }
    }
    GPIOC->BSRR = ((ACTIVITY_PINS | RGB_PINS) & ~on) | (on << 16U);
}

void ailink_product_hw_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN;
    (void)RCC->AHB1ENR;
    /* Write inactive levels before changing the output modes. */
    GPIOC->BSRR = ACTIVITY_PINS | RGB_PINS | (POWER_PIN << 16U);
    uint32_t pins = ACTIVITY_PINS | RGB_PINS | POWER_PIN;
    uint32_t mask = mode_mask((uint16_t)pins);
    uint32_t outputs = mask & 0x55555555UL;
    GPIOC->MODER = (GPIOC->MODER & ~mask) | outputs;
    GPIOC->PUPDR &= ~mask;
    GPIOC->OTYPER &= ~pins;
    target_ready = false;
    ailink_target_power_stop();
}
