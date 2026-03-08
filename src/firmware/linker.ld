/*
 * Linker script for PocketSM64 on Analogue Pocket
 *
 * Two-stage boot:
 * - Bootloader: 64KB BRAM at 0x00000000 (initialized from firmware.mif)
 * - App binary: deferload from SD card into SDRAM at 0x10200000
 *   (runs in-place from SDRAM, no PSRAM copy)
 *
 * Memory layout:
 * - BRAM:     0x00000000 (64KB)  - Bootloader code, stack
 * - SDRAM:    0x10000000 (64MB)  - App code + data + BSS + Heap
 *   - 0x10200000: sm64.bin (deferloaded, code+rodata+data)
 *   - After sm64.bin: BSS + Heap
 *   - 0x13000000: Runtime stack (512KB, grows down)
 *   - 0x13E00000: Dataslot param/resp buffers
 *   - 0x13F00000: DMA bounce buffer
 * - Terminal: 0x20000000 (8KB)   - Character VRAM
 * - PSRAM:    0x30000000 (16MB)  - Available (CRAM0, unused for now)
 * - SRAM:     0x38000000 (256KB) - Z-buffer
 * - SysRegs:  0x40000000         - System control registers
 */

ENTRY(_start)

MEMORY {
    BRAM      (rwx) : ORIGIN = 0x00000000, LENGTH = 64K
    SDRAM_APP (rwx) : ORIGIN = 0x10200000, LENGTH = 44M   /* App code + BSS + heap */
}

/* SDRAM layout symbols */
__sdram_base = 0x10000000;
__sdram_size = 64M;

/* App binary: both VMA and LMA in SDRAM (deferloaded, runs in-place) */
__app_load_addr = 0x10200000;

SECTIONS {
    /* Bootloader code - in BRAM, runs at startup */
    .boot : {
        KEEP(*(.text.start))       /* Startup code first */
        *(.text.boot*)             /* Bootloader functions */
        *terminal.o(.text*)        /* Terminal functions for boot diagnostics */
        *terminal.o(.rodata*)
        *terminal.o(.srodata*)
        *dataslot.o(.text*)        /* Dataslot functions for loading */
        *dataslot.o(.rodata*)
        *dataslot.o(.srodata*)
        *main.o(.srodata*)         /* Boot test float constants */
        *(.rodata.boot*)
        . = ALIGN(4);
    } > BRAM

    /* Bootloader data */
    .boot_data : {
        *(.data.boot*)
        *(.sdata.boot*)
        . = ALIGN(4);
    } > BRAM

    /* Bootloader BSS */
    .boot_bss : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss.boot*)
        *(.sbss.boot*)
        . = ALIGN(4);
        __bss_end = .;
    } > BRAM

    /* Hot app code/data pinned in BRAM for lower latency. */
    .fasttext : {
        . = ALIGN(4);
        __fasttext_start = .;
        KEEP(*(.fasttext*))
        KEEP(*(.fastrodata*))
        . = ALIGN(4);
        __fasttext_end = .;
    } > BRAM

    .fastdata : {
        . = ALIGN(4);
        __fastdata_start = .;
        KEEP(*(.fastdata*))
        . = ALIGN(4);
        __fastdata_end = .;
    } > BRAM

    /* === SDRAM sections: App code runs in-place from SDRAM === */

    /* App code section */
    .text __app_load_addr : {
        __text_start = .;
        KEEP(*(.text*))
        KEEP(*(.rodata*))
        KEEP(*(.srodata*))
        . = ALIGN(4);
        __text_end = .;
    } > SDRAM_APP

    /* App initialized data */
    .data : {
        __data_start = .;
        KEEP(*(.data*))
        KEEP(*(.sdata*))
        . = ALIGN(4);
        __data_end = .;
    } > SDRAM_APP

    /* Size of code+data that must be deferloaded */
    __app_load_size = __data_end - __text_start;

    /* App BSS - immediately after data in SDRAM */
    .qbss : {
        . = ALIGN(4);
        __qbss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(4);
        __qbss_end = .;
    } > SDRAM_APP

    /* Heap starts after BSS */
    __heap_start = __qbss_end;
    __runtime_stack_top = 0x13000000;
    __runtime_stack_size = 512K;
    __runtime_stack_bottom = __runtime_stack_top - __runtime_stack_size;
    __heap_end = __runtime_stack_bottom;

    /* Stack at end of BRAM (grows downward) */
    __stack_top = ORIGIN(BRAM) + LENGTH(BRAM);

    /* Provide symbols for C code */
    PROVIDE(_start_addr = ORIGIN(BRAM));
    PROVIDE(_stack_top = __stack_top);
    PROVIDE(_bss_start = __bss_start);
    PROVIDE(_bss_end = __bss_end);
    PROVIDE(_qbss_start = __qbss_start);
    PROVIDE(_qbss_end = __qbss_end);
    PROVIDE(_heap_start = __heap_start);
    PROVIDE(_heap_end = __heap_end);
    PROVIDE(_runtime_stack_top = __runtime_stack_top);
    PROVIDE(_app_load_addr = __app_load_addr);
    PROVIDE(_app_load_size = __app_load_size);

    /* Keep at least 8 KB BRAM stack/trap headroom after hot sections. */
    ASSERT(__fastdata_end <= (__stack_top - 8192), "BRAM overflow: insufficient stack headroom")
}
