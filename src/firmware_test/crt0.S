/*
 * Minimal C runtime startup for VexRiscv
 * Sets up stack and calls main()
 */

.section .text.start
.global _start

_start:
    /* Set up stack pointer at end of RAM */
    lui sp, %hi(_stack_top)
    addi sp, sp, %lo(_stack_top)

    /* Clear BSS section */
    lui a0, %hi(_bss_start)
    addi a0, a0, %lo(_bss_start)
    lui a1, %hi(_bss_end)
    addi a1, a1, %lo(_bss_end)
clear_bss:
    bge a0, a1, bss_done
    sw zero, 0(a0)
    addi a0, a0, 4
    j clear_bss
bss_done:

    /* Call main */
    jal main

    /* Halt if main returns */
halt:
    j halt
