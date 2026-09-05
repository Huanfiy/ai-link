/* Product host-test peripheral boundary. SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
typedef struct
{
    uint32_t MODER, PUPDR, OTYPER, OSPEEDR, AFR[2], BSRR;
} GPIO_TypeDef;
typedef struct
{
    uint32_t AHB1ENR;
} RCC_TypeDef;
extern GPIO_TypeDef test_gpio[3];
extern RCC_TypeDef test_rcc;
#define GPIOA               (&test_gpio[0])
#define GPIOB               (&test_gpio[1])
#define GPIOC               (&test_gpio[2])
#define RCC                 (&test_rcc)
#define RCC_AHB1ENR_GPIOAEN 1U
#define RCC_AHB1ENR_GPIOBEN 2U
#define RCC_AHB1ENR_GPIOCEN 4U
