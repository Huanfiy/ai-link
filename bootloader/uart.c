/*
 * uart.c - USART2 (PA2) TX-only polled output at 115200 8N1.
 *
 * Boot console shares the PA2/PA3 header with the app's finsh console so one
 * external adapter sees the whole boot flow. Clocked straight off the
 * reset-default HSI 16 MHz (APB1 prescaler is 1 at reset): BRR = 16e6/115200
 * = 138.9 -> 139 (0x8B, USARTDIV 8.6875), -0.08% error. uart_deinit() puts
 * USART2 and GPIOA back through their RCC reset lines so the ROM DFU jump
 * still sees a near-reset peripheral state (GPIOA also carries the OTG_FS
 * pins the ROM bootloader owns).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "uart.h"
#include "regs.h"

#define UART_BRR_HSI16_115200 0x8BU

void uart_init(void)
{
    RCC_AHB1ENR |= RCC_AHB1_GPIOA;
    RCC_APB1ENR |= RCC_APB1_USART2;

    /* PA2 = AF7 (USART2_TX), push-pull, default speed is plenty for 115200 */
    GPIOA_AFRL = (GPIOA_AFRL & ~(0xFU << 8)) | (7U << 8);
    GPIOA_MODER = (GPIOA_MODER & ~(3U << 4)) | (2U << 4);

    USART2_BRR = UART_BRR_HSI16_115200;
    USART2_CR1 = USART_CR1_UE | USART_CR1_TE;
}

static void uart_putc(char c)
{
    while (!(USART2_SR & USART_SR_TXE)) {
    }
    USART2_DR = (uint8_t)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

void uart_puthex(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    uart_putc('0');
    uart_putc('x');
    for (int shift = 28; shift >= 0; shift -= 4) {
        uart_putc(digits[(value >> shift) & 0xFU]);
    }
}

void uart_putsn(const char *s, uint32_t max_len)
{
    while (max_len-- && *s) {
        uart_putc(*s++);
    }
}

void uart_deinit(void)
{
    /* wait for the shift register to drain before resetting the peripheral */
    while (!(USART2_SR & USART_SR_TC)) {
    }

    RCC_APB1RSTR |= RCC_APB1_USART2;
    RCC_APB1RSTR &= ~RCC_APB1_USART2;
    RCC_AHB1RSTR |= RCC_AHB1_GPIOA;
    RCC_AHB1RSTR &= ~RCC_AHB1_GPIOA;

    RCC_APB1ENR &= ~RCC_APB1_USART2;
    RCC_AHB1ENR &= ~RCC_AHB1_GPIOA;
}
