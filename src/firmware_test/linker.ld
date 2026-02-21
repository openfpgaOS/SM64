/*
 * Linker script for VexRiscv minimal Hello World
 *
 * Memory layout:
 * - RAM:      0x00000000 (64KB) - Code and stack
 * - Terminal: 0x20000000 (8KB)  - Character VRAM
 * - SysRegs:  0x40000000 (32B)  - System control registers
 */

ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 64K
}

SECTIONS {
    /* Code section - starts at 0 */
    .text : {
        KEEP(*(.text.start))   /* Startup code first */
        *(.text*)              /* All other code */
        *(.rodata*)            /* Read-only data */
        . = ALIGN(4);
    } > RAM

    /* Initialized data */
    .data : {
        *(.data*)
        *(.sdata*)
        . = ALIGN(4);
    } > RAM

    /* Uninitialized data (BSS) */
    .bss : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    /* Stack at end of RAM (grows downward) */
    __stack_top = ORIGIN(RAM) + LENGTH(RAM);

    /* Provide symbols for assembly code */
    PROVIDE(_stack_top = __stack_top);
    PROVIDE(_bss_start = __bss_start);
    PROVIDE(_bss_end = __bss_end);
}
