/*
 * main.c - boot decision flow (see docs/design/ota.md).
 *
 * Runs on the reset-default HSI 16 MHz with interrupts off and (almost) no
 * peripherals touched, so both exit paths keep their contract: the ROM DFU
 * jump requires a near-reset environment (AN2606), and the app expects to
 * own the full clock bring-up. Decision order:
 *
 *   1. BKP0R == 0x5AFEB007 (host-requested DFU): clear flag, jump ROM DFU;
 *   2. .fw_info at app base + 0x200 validates: set VTOR, jump app;
 *   3. otherwise (truncated/corrupt/erased app): LED1 on, jump ROM DFU,
 *      so a bricked app is recoverable with nothing but the USB cable.
 *
 * STM32F446 system memory maps at the same 0x1FFF0000 as F407 (AN2606
 * pattern), so the ROM DFU jump is unchanged.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>

#include "regs.h"
#include "crc32.h"
#include "uart.h"

#include "fw_info.h" /* applications/ota/: shared 64 B header layout */

#define APP_BASE     0x08010000UL
#define APP_MAX_SIZE (448UL * 1024UL)

#define DFU_TRAMPOLINE_MAGIC 0x5AFEB007UL
#define ROM_DFU_VECTOR_BASE  0x1FFF0000UL /* system memory, AN2606 pattern 0x91 */

void boot_main(void) __attribute__((noreturn));

__attribute__((noreturn))
static void jump_via_vector(uint32_t vector_base)
{
    uint32_t msp = ((const uint32_t *)vector_base)[0];
    uint32_t entry = ((const uint32_t *)vector_base)[1];

    __asm volatile("msr msp, %0\n\t"
                   "bx  %1\n\t"
                   :
                   : "r"(msp), "r"(entry));
    __builtin_unreachable();
}

__attribute__((noreturn))
static void jump_rom_dfu(void)
{
    /* Alias system memory at 0x00000000 (boot-from-system-memory layout)
     * so the ROM sees its vectors wherever it expects them; VTOR is still
     * at its reset value 0 in every path reaching here. */
    RCC_APB2ENR |= RCC_APB2_SYSCFG;
    SYSCFG_MEMRMP = 0x1U;

    jump_via_vector(ROM_DFU_VECTOR_BASE);
}

/* Read BKP0R and clear it when armed: the trampoline must fire exactly once,
 * otherwise a DFU-then-reset cycle could loop back into DFU forever. */
static uint32_t dfu_flag_consume(void)
{
    uint32_t flag;

    RCC_APB1ENR |= RCC_APB1_PWR;
    PWR_CR |= PWR_CR_DBP;

    flag = RTC_BKP0R;
    if (flag == DFU_TRAMPOLINE_MAGIC) {
        RTC_BKP0R = 0;
    }

    PWR_CR &= ~PWR_CR_DBP;
    RCC_APB1ENR &= ~RCC_APB1_PWR;
    return flag;
}

/* Validate the app image in place against its .fw_info header.
 * Returns NULL when valid, otherwise a short reason for the log. */
static const char *app_validate(void)
{
    const struct fw_info *info = (const struct fw_info *)(APP_BASE + FW_INFO_OFFSET);
    const uint8_t *base = (const uint8_t *)APP_BASE;
    uint32_t payload_start = FW_INFO_OFFSET + sizeof(struct fw_info);
    uint32_t crc;

    if (info->magic != FW_INFO_MAGIC) {
        return "no fw_info magic";
    }
    crc = crc32_update(0, info, sizeof(*info) - sizeof(uint32_t));
    if (crc != info->header_crc32) {
        return "header CRC mismatch";
    }
    if (info->board_id != FW_INFO_BOARD_ID) {
        return "board_id mismatch";
    }
    if (info->image_size <= payload_start || info->image_size > APP_MAX_SIZE) {
        return "image_size out of range";
    }

    /* image CRC skips the 64 B header itself: [0, 0x200) + [0x240, size) */
    crc = crc32_update(0, base, FW_INFO_OFFSET);
    crc = crc32_update(crc, base + payload_start, info->image_size - payload_start);
    if (crc != info->image_crc32) {
        return "image CRC mismatch";
    }
    return 0;
}

/* LED1 (PC0, active low) marks the invalid-app fallback; it stays lit
 * through the ROM DFU session as the visible "recovery mode" indicator. */
static void led_fault_on(void)
{
    RCC_AHB1ENR |= RCC_AHB1_GPIOC;
    GPIOC_MODER = (GPIOC_MODER & ~0x3U) | 0x1U;
    GPIOC_BSRR = (1U << 16);
}

void boot_main(void)
{
    const char *reason;

    if (dfu_flag_consume() == DFU_TRAMPOLINE_MAGIC) {
        uart_init();
        uart_puts("\n[boot] host DFU request -> ROM DFU\n");
        uart_deinit();
        jump_rom_dfu();
    }

    uart_init();
    crc32_init();

    reason = app_validate();
    if (reason == 0) {
        const struct fw_info *info = (const struct fw_info *)(APP_BASE + FW_INFO_OFFSET);

        uart_puts("\n[boot] app ");
        uart_putsn(info->fw_version, FW_INFO_VER_LEN);
        uart_puts(" ok -> jump\n");
        uart_deinit();

        SCB_VTOR = APP_BASE;
        jump_via_vector(APP_BASE);
    }

    uart_puts("\n[boot] invalid app (");
    uart_puts(reason);
    uart_puts(") -> ROM DFU\n");
    led_fault_on();
    uart_deinit();
    jump_rom_dfu();
}
