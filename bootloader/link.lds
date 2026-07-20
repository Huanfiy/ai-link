/*
 * Linker script for the ailink bare-metal bootloader.
 * Occupies flash sectors 0-3 (64 KB); the RT-Thread app starts at 0x08010000.
 */
MEMORY
{
    ROM (rx) : ORIGIN = 0x08000000, LENGTH = 64K
    RAM (rw) : ORIGIN = 0x20000000, LENGTH = 128K
}
ENTRY(Reset_Handler)

SECTIONS
{
    .text :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > ROM

    _sidata = _etext;

    .data : AT (_sidata)
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* MSP starts at the top of SRAM1; the bootloader uses a few hundred bytes */
    _estack = ORIGIN(RAM) + LENGTH(RAM);

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) }
}
