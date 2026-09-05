/*
 * dap_config.h - CMSIS-DAP hardware I/O layer for ailink (SWD only).
 *
 * Board-level pin mapping (register-level BSRR/MODER bit operations):
 *   SWCLK/TCK = PA4 (push-pull output)
 *   SWDIO/TMS = PA5 (push-pull output <-> floating input)
 *   nRESET    = PA6 (open-drain output)
 *
 * API surface follows the ARM CMSIS-DAP reference DAP_config.h template
 * (Apache-2.0), stripped to SWD: DAP_JTAG=0, no SWO, no UART.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __DAP_CONFIG_H__
#define __DAP_CONFIG_H__

#include "ailink_product.h"
#include <rtthread.h>
#include <stdint.h>

#include <stm32f4xx.h>

#include "cmsis_compiler.h"

/// Processor Clock of the Cortex-M MCU used in the Debug Unit.
#define CPU_CLOCK 180000000U

/// Number of processor cycles for I/O Port write operations (AHB1 GPIO).
#define IO_PORT_WRITE_CYCLES 2U

/// Indicate that Serial Wire Debug (SWD) communication mode is available.
#define DAP_SWD 1

/// Indicate that JTAG communication mode is available (not wired out).
#define DAP_JTAG 0

/// Configure maximum number of JTAG devices on the scan chain.
#define DAP_JTAG_DEV_CNT 0U

/// Default communication mode on the Debug Access Port.
#define DAP_DEFAULT_PORT 1U /* SWD */

/// Default communication speed on the Debug Access Port for SWD mode (Hz).
#define DAP_DEFAULT_SWJ_CLOCK 1000000U

/// Maximum packet size for the communication endpoint (FS bulk).
#define DAP_PACKET_SIZE 64U

/// Maximum packet buffers for the command/response queue.
#define DAP_PACKET_COUNT 4U

/// Indicate that UART Serial Wire Output (SWO) trace is available.
#define SWO_UART 0

/// USART instance handling (unused, SWO disabled).
#define SWO_UART_DRIVER 0

/// Maximum SWO UART baudrate.
#define SWO_UART_MAX_BAUDRATE 0U

/// Indicate that Manchester Serial Wire Output (SWO) trace is available.
#define SWO_MANCHESTER 0

/// SWO trace buffer size.
#define SWO_BUFFER_SIZE 0U

/// SWO streaming trace.
#define SWO_STREAM 0

/// Clock frequency of the Test Domain Timer (DWT cycle counter).
#define TIMESTAMP_CLOCK 180000000U

/// Indicate that UART Communication Port is available.
#define DAP_UART 0

/// USART instance for UART communication port (unused).
#define DAP_UART_DRIVER 0

/// UART Receive Buffer Size.
#define DAP_UART_RX_BUFFER_SIZE 0U

/// UART Transmit Buffer Size.
#define DAP_UART_TX_BUFFER_SIZE 0U

/// Indicate that UART Communication via USB COM Port is available.
#define DAP_UART_USB_COM_PORT 0

/// Debug Unit is transferred to a slower default clock when this is 1.
#define DAP_DEFAULT_SWJ_CLOCK_SLOW 0

/* ---------------- pin helpers (GPIOA, register level) ---------------- */

#define DAP_GPIO          GPIOA
#define DAP_PIN_SWCLK     4U
#define DAP_PIN_SWDIO     5U
#define DAP_PIN_NRESET    6U

/* MODER field manipulation for one pin: 00 input, 01 output */
__STATIC_FORCEINLINE void dap_pin_mode_output(uint32_t pin)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (ailink_target_is_ready())
    {
        DAP_GPIO->MODER = (DAP_GPIO->MODER & ~(3UL << (pin * 2U))) | (1UL << (pin * 2U));
    }
    rt_hw_interrupt_enable(level);
}

__STATIC_FORCEINLINE void dap_pin_mode_input(uint32_t pin)
{
    DAP_GPIO->MODER &= ~(3UL << (pin * 2U));
}

/* ---------------- CMSIS-DAP required I/O functions ---------------- */

/** Setup JTAG I/O pins: TCK, TMS, TDI, TDO, nTRST, and nRESET (unused). */
__STATIC_INLINE void PORT_JTAG_SETUP(void)
{
}

/** Setup SWD I/O pins: SWCLK, SWDIO, and nRESET. */
__STATIC_INLINE void PORT_SWD_SETUP(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!ailink_target_is_ready())
    {
        rt_hw_interrupt_enable(level);
        return;
    }
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    (void)RCC->AHB1ENR;

    /* SWCLK/SWDIO high, nRESET released (open-drain high) */
    DAP_GPIO->BSRR = (1UL << DAP_PIN_SWCLK) | (1UL << DAP_PIN_SWDIO) |
                     (1UL << DAP_PIN_NRESET);

    /* high speed, SWCLK/SWDIO push-pull, nRESET open-drain */
    DAP_GPIO->OSPEEDR |= (3UL << (DAP_PIN_SWCLK * 2U)) |
                         (3UL << (DAP_PIN_SWDIO * 2U));
    DAP_GPIO->OTYPER &= ~((1UL << DAP_PIN_SWCLK) | (1UL << DAP_PIN_SWDIO));
    DAP_GPIO->OTYPER |= (1UL << DAP_PIN_NRESET);
    /* SWDIO pulled up (SWD host convention: keep the line defined during
     * turnaround when neither host nor target drives it); nRESET pulled up
     * so the open-drain release idles high; SWCLK is always host-driven. */
    DAP_GPIO->PUPDR &= ~((3UL << (DAP_PIN_SWCLK * 2U)) |
                         (3UL << (DAP_PIN_SWDIO * 2U)) |
                         (3UL << (DAP_PIN_NRESET * 2U)));
    DAP_GPIO->PUPDR |= (1UL << (DAP_PIN_SWDIO * 2U)) |
                       (1UL << (DAP_PIN_NRESET * 2U));

    dap_pin_mode_output(DAP_PIN_SWCLK);
    dap_pin_mode_output(DAP_PIN_SWDIO);
    dap_pin_mode_output(DAP_PIN_NRESET);
    rt_hw_interrupt_enable(level);
}

/** Disable JTAG/SWD I/O Pins: all to high-impedance. */
__STATIC_INLINE void PORT_OFF(void)
{
    dap_pin_mode_input(DAP_PIN_SWCLK);
    dap_pin_mode_input(DAP_PIN_SWDIO);
    dap_pin_mode_input(DAP_PIN_NRESET);
    DAP_GPIO->PUPDR &= ~((3UL << (DAP_PIN_SWCLK * 2U)) | (3UL << (DAP_PIN_SWDIO * 2U)) |
                         (3UL << (DAP_PIN_NRESET * 2U)));
}

/** SWCLK/TCK I/O pin: Get Input. */
__STATIC_FORCEINLINE uint32_t PIN_SWCLK_TCK_IN(void)
{
    return (DAP_GPIO->IDR >> DAP_PIN_SWCLK) & 1U;
}

/** SWCLK/TCK I/O pin: Set Output to High. */
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_SET(void)
{
    DAP_GPIO->BSRR = (1UL << DAP_PIN_SWCLK);
}

/** SWCLK/TCK I/O pin: Set Output to Low. */
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_CLR(void)
{
    DAP_GPIO->BSRR = (1UL << (DAP_PIN_SWCLK + 16U));
}

/** SWDIO/TMS I/O pin: Get Input. */
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_TMS_IN(void)
{
    return (DAP_GPIO->IDR >> DAP_PIN_SWDIO) & 1U;
}

/** SWDIO/TMS I/O pin: Set Output to High. */
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_SET(void)
{
    DAP_GPIO->BSRR = (1UL << DAP_PIN_SWDIO);
}

/** SWDIO/TMS I/O pin: Set Output to Low. */
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_CLR(void)
{
    DAP_GPIO->BSRR = (1UL << (DAP_PIN_SWDIO + 16U));
}

/** SWDIO I/O pin: Get Input (used in SWD mode only). */
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_IN(void)
{
    return (DAP_GPIO->IDR >> DAP_PIN_SWDIO) & 1U;
}

/** SWDIO I/O pin: Set Output (used in SWD mode only). */
__STATIC_FORCEINLINE void PIN_SWDIO_OUT(uint32_t bit)
{
    if (bit & 1U) {
        DAP_GPIO->BSRR = (1UL << DAP_PIN_SWDIO);
    } else {
        DAP_GPIO->BSRR = (1UL << (DAP_PIN_SWDIO + 16U));
    }
}

/** SWDIO I/O pin: Switch to Output mode (used in SWD mode only). */
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_ENABLE(void)
{
    dap_pin_mode_output(DAP_PIN_SWDIO);
}

/** SWDIO I/O pin: Switch to Input mode (used in SWD mode only). */
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_DISABLE(void)
{
    dap_pin_mode_input(DAP_PIN_SWDIO);
}

/** TDI I/O pin: Get Input (JTAG not wired). */
__STATIC_FORCEINLINE uint32_t PIN_TDI_IN(void)
{
    return 0U;
}

/** TDI I/O pin: Set Output (JTAG not wired). */
__STATIC_FORCEINLINE void PIN_TDI_OUT(uint32_t bit)
{
    (void)bit;
}

/** TDO I/O pin: Get Input (JTAG not wired). */
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void)
{
    return 0U;
}

/** nTRST I/O pin: Get Input (JTAG not wired). */
__STATIC_FORCEINLINE uint32_t PIN_nTRST_IN(void)
{
    return 0U;
}

/** nTRST I/O pin: Set Output (JTAG not wired). */
__STATIC_FORCEINLINE void PIN_nTRST_OUT(uint32_t bit)
{
    (void)bit;
}

/** nRESET I/O pin: Get Input. */
__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN(void)
{
    return (DAP_GPIO->IDR >> DAP_PIN_NRESET) & 1U;
}

/** nRESET I/O pin: Set Output (open drain: 0 = assert reset). */
__STATIC_FORCEINLINE void PIN_nRESET_OUT(uint32_t bit)
{
    if (bit & 1U) {
        DAP_GPIO->BSRR = (1UL << DAP_PIN_NRESET);
    } else {
        DAP_GPIO->BSRR = (1UL << (DAP_PIN_NRESET + 16U));
    }
}

/** Debug Unit: Set status of Connected LED (not wired, no-op). */
__STATIC_INLINE void LED_CONNECTED_OUT(uint32_t bit)
{
    (void)bit;
}

/** Debug Unit: Set status of Target Running LED (not wired, no-op). */
__STATIC_INLINE void LED_RUNNING_OUT(uint32_t bit)
{
    (void)bit;
}

/** Get timestamp of Test Domain Timer (DWT cycle counter). */
__STATIC_INLINE uint32_t TIMESTAMP_GET(void)
{
    return DWT->CYCCNT;
}

/** Setup of the Debug Unit I/O pins and LEDs (called when Debug Unit is initialized). */
__STATIC_INLINE void DAP_SETUP(void)
{
    /* enable DWT cycle counter for TIMESTAMP_GET */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    PORT_OFF();
}

/** Reset Target Device with custom specific I/O pin or command sequence.
 *  Return 0 = no device specific reset sequence implemented. */
__STATIC_INLINE uint8_t RESET_TARGET(void)
{
    return 0U;
}

#endif /* __DAP_CONFIG_H__ */
