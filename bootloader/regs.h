/*
 * regs.h - the handful of STM32F446 registers the bootloader touches.
 *
 * Self-contained on purpose: the bootloader is flashed once and never updated
 * over OTA, so it must not depend on the pkgs-managed CMSIS/HAL trees
 * (packages/ is not checked in and may move underneath it).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOOT_REGS_H
#define BOOT_REGS_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* RCC */
#define RCC_AHB1RSTR REG32(0x40023810)
#define RCC_APB1RSTR REG32(0x40023820)
#define RCC_AHB1ENR  REG32(0x40023830)
#define RCC_APB1ENR  REG32(0x40023840)
#define RCC_APB2ENR  REG32(0x40023844)

#define RCC_AHB1_GPIOA (1U << 0)
#define RCC_AHB1_GPIOC (1U << 2)
#define RCC_APB1_PWR   (1U << 28)
#define RCC_APB1_USART2 (1U << 17)

/* PWR: DBP unlocks backup-domain writes (RTC backup registers) */
#define PWR_CR     REG32(0x40007000)
#define PWR_CR_DBP (1U << 8)

/* RTC backup register 0: DFU trampoline mailbox, survives NVIC_SystemReset */
#define RTC_BKP0R REG32(0x40002850)

/* SYSCFG: memory remap for the ROM DFU jump */
#define SYSCFG_MEMRMP   REG32(0x40013800)
#define RCC_APB2_SYSCFG (1U << 14)

/* GPIO */
#define GPIOA_MODER  REG32(0x40020000)
#define GPIOA_AFRL   REG32(0x40020020)
#define GPIOC_MODER  REG32(0x40020800)
#define GPIOC_BSRR   REG32(0x40020818)

/* USART2 (PA2 TX), fed by APB1 = HSI 16 MHz in the bootloader */
#define USART2_SR  REG32(0x40004400)
#define USART2_DR  REG32(0x40004404)
#define USART2_BRR REG32(0x40004408)
#define USART2_CR1 REG32(0x4000440C)

#define USART_SR_TC   (1U << 6)
#define USART_SR_TXE  (1U << 7)
#define USART_CR1_TE  (1U << 3)
#define USART_CR1_UE  (1U << 13)

/* Cortex-M4 SCB */
#define SCB_VTOR REG32(0xE000ED08)

#endif /* BOOT_REGS_H */
