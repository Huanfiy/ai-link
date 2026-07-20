/*
 * startup.c - C vector table and reset handler for the bootloader.
 *
 * Only the 16 core vectors are populated: the bootloader never enables
 * interrupts, so peripheral IRQs cannot fire. Faults spin in place, which
 * a debugger can inspect; the independent watchdog is not started, matching
 * the near-reset state the ROM DFU jump requires.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;

void boot_main(void) __attribute__((noreturn));
void Reset_Handler(void) __attribute__((noreturn));

static void Default_Handler(void)
{
    for (;;) {
    }
}

__attribute__((used, section(".isr_vector")))
static void (*const vector_table[16])(void) = {
    (void (*)(void))((uintptr_t)&_estack),
    Reset_Handler,
    Default_Handler, /* NMI */
    Default_Handler, /* HardFault */
    Default_Handler, /* MemManage */
    Default_Handler, /* BusFault */
    Default_Handler, /* UsageFault */
    0, 0, 0, 0,      /* reserved */
    Default_Handler, /* SVCall */
    Default_Handler, /* DebugMonitor */
    0,               /* reserved */
    Default_Handler, /* PendSV */
    Default_Handler, /* SysTick */
};

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;

    while (dst < &_edata) {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss; dst++) {
        *dst = 0;
    }

    boot_main();
}
