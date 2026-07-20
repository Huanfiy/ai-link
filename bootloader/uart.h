/*
 * uart.h - UART1 TX-only polled console for boot decision logs.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOOT_UART_H
#define BOOT_UART_H

#include <stdint.h>

/** @brief UART1 on PA9, 115200 8N1, HSI 16 MHz APB2 clock, TX polling only. */
void uart_init(void);

void uart_puts(const char *s);

/** @brief Print "0x" + 8 hex digits. */
void uart_puthex(uint32_t value);

/** @brief Print at most max_len chars of a possibly unterminated string. */
void uart_putsn(const char *s, uint32_t max_len);

/** @brief Drain the shifter, then reset USART1 and GPIOA back to POR state. */
void uart_deinit(void);

#endif /* BOOT_UART_H */
