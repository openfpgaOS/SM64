/*
 * VexiiRiscv startup code
 * Sets up stack pointer and jumps to main
 * Includes trap handler for misaligned access emulation and crash debugging
 *
 * Trap handling uses mscratch to hold the BRAM trap stack pointer.
 * On trap entry, csrrw atomically swaps sp <-> mscratch, giving us a
 * safe stack without clobbering the original sp.
 */

.section .text.start
.global _start

_start:
    /* Cold boot: set up stack pointer */
    lui sp, %hi(_stack_top)
    addi sp, sp, %lo(_stack_top)

    /* Enable FPU: set mstatus.FS = 11 (Dirty) — bits [14:13] */
    li t0, (3 << 13)
    csrs mstatus, t0

    /* Set mscratch = trap stack top (used by trap vector for sp swap) */
    csrw mscratch, sp

    /* Set mtvec to trap vector (separate from cold boot entry) */
    la t0, _trap_vector
    csrw mtvec, t0

    /* Mark that we've booted (store magic value at fixed BRAM address) */
    lui t0, 0x0000F     /* Use top of BRAM area 0x0000F000 as scratch */
    li t1, 0xDEAD0001
    sw t1, 0(t0)

    /* Clear BSS section */
    lui a0, %hi(_bss_start)
    addi a0, a0, %lo(_bss_start)
    lui a1, %hi(_bss_end)
    addi a1, a1, %lo(_bss_end)
clear_bss:
    bge a0, a1, done_bss
    sw zero, 0(a0)
    addi a0, a0, 4
    j clear_bss
done_bss:

    /* Jump to main */
    jal ra, main

    /* If main returns, loop forever */
halt:
    j halt


/* ========================================
 * Trap vector — entered via mtvec on any exception.
 *
 * mscratch holds the BRAM trap stack top.
 * csrrw atomically swaps sp <-> mscratch so we get a safe stack
 * and preserve the original sp in mscratch.
 *
 * Supports nested traps: after saving original sp, we restore
 * mscratch to the new trap stack top (frame allocated below it).
 *
 * Trap frame layout:
 *   x0-x31  (32 words) at sp+0   .. sp+124
 *   mepc    (1 word)   at sp+128
 *   mcause  (1 word)   at sp+132
 *   mtval   (1 word)   at sp+136
 *   f0-f31  (32 words) at sp+140 .. sp+264
 * Total: 67 words = 268 bytes (round to 272 for 8-byte alignment)
 * ======================================== */
_trap_vector:
    /* Atomically swap sp <-> mscratch.
     * After this: sp = BRAM trap stack top, mscratch = original sp */
    csrrw sp, mscratch, sp

    /* Allocate trap frame: 272 bytes */
    addi sp, sp, -272

    /* Save t0 first — we need it as a working register */
    sw x5,  20(sp)

    /* Save original sp from mscratch */
    csrr x5, mscratch
    sw x5,   8(sp)

    /* Restore mscratch = current trap stack top for nested trap safety.
     * If a nested trap occurs (e.g. in term_printf), it will allocate
     * another frame below this one on the same BRAM stack. */
    mv x5, sp
    csrw mscratch, x5

    /* Save x1, x3-x4 (x0 is always 0, x2/sp saved above, x5/t0 saved above) */
    sw x1,   4(sp)
    sw x3,  12(sp)
    sw x4,  16(sp)
    /* x5 (t0) already saved at offset 20 */
    sw x6,  24(sp)
    sw x7,  28(sp)
    sw x8,  32(sp)
    sw x9,  36(sp)
    sw x10, 40(sp)
    sw x11, 44(sp)
    sw x12, 48(sp)
    sw x13, 52(sp)
    sw x14, 56(sp)
    sw x15, 60(sp)
    sw x16, 64(sp)
    sw x17, 68(sp)
    sw x18, 72(sp)
    sw x19, 76(sp)
    sw x20, 80(sp)
    sw x21, 84(sp)
    sw x22, 88(sp)
    sw x23, 92(sp)
    sw x24, 96(sp)
    sw x25, 100(sp)
    sw x26, 104(sp)
    sw x27, 108(sp)
    sw x28, 112(sp)
    sw x29, 116(sp)
    sw x30, 120(sp)
    sw x31, 124(sp)

    /* Save CSRs: mepc, mcause, mtval */
    csrr t0, mepc
    sw t0, 128(sp)
    csrr t0, mcause
    sw t0, 132(sp)
    csrr t0, mtval
    sw t0, 136(sp)

    /* Store 0 at x0 position (for completeness) */
    sw zero, 0(sp)

    /* Save f0-f31 (floating-point registers) */
    fsw f0,  140(sp)
    fsw f1,  144(sp)
    fsw f2,  148(sp)
    fsw f3,  152(sp)
    fsw f4,  156(sp)
    fsw f5,  160(sp)
    fsw f6,  164(sp)
    fsw f7,  168(sp)
    fsw f8,  172(sp)
    fsw f9,  176(sp)
    fsw f10, 180(sp)
    fsw f11, 184(sp)
    fsw f12, 188(sp)
    fsw f13, 192(sp)
    fsw f14, 196(sp)
    fsw f15, 200(sp)
    fsw f16, 204(sp)
    fsw f17, 208(sp)
    fsw f18, 212(sp)
    fsw f19, 216(sp)
    fsw f20, 220(sp)
    fsw f21, 224(sp)
    fsw f22, 228(sp)
    fsw f23, 232(sp)
    fsw f24, 236(sp)
    fsw f25, 240(sp)
    fsw f26, 244(sp)
    fsw f27, 248(sp)
    fsw f28, 252(sp)
    fsw f29, 256(sp)
    fsw f30, 260(sp)
    fsw f31, 264(sp)

    /* Call C handler: handle_misaligned(trap_frame_t *frame)
     * Returns 1 if handled (should return), 0 if fatal */
    mv a0, sp
    jal ra, handle_misaligned

    /* Check return value */
    beqz a0, _fatal_trap

    /* Handled - restore mepc (may have been modified) */
    lw t0, 128(sp)
    csrw mepc, t0

    /* Restore f0-f31 (may have been modified by FLW emulation) */
    flw f0,  140(sp)
    flw f1,  144(sp)
    flw f2,  148(sp)
    flw f3,  152(sp)
    flw f4,  156(sp)
    flw f5,  160(sp)
    flw f6,  164(sp)
    flw f7,  168(sp)
    flw f8,  172(sp)
    flw f9,  176(sp)
    flw f10, 180(sp)
    flw f11, 184(sp)
    flw f12, 188(sp)
    flw f13, 192(sp)
    flw f14, 196(sp)
    flw f15, 200(sp)
    flw f16, 204(sp)
    flw f17, 208(sp)
    flw f18, 212(sp)
    flw f19, 216(sp)
    flw f20, 220(sp)
    flw f21, 224(sp)
    flw f22, 228(sp)
    flw f23, 232(sp)
    flw f24, 236(sp)
    flw f25, 240(sp)
    flw f26, 244(sp)
    flw f27, 248(sp)
    flw f28, 252(sp)
    flw f29, 256(sp)
    flw f30, 260(sp)
    flw f31, 264(sp)

    /* Restore integer registers (except sp and t0, handled below) */
    lw x1,   4(sp)
    /* x2 (sp) restored via csrrw below */
    lw x3,  12(sp)
    lw x4,  16(sp)
    /* x5 (t0) restored last */
    lw x6,  24(sp)
    lw x7,  28(sp)
    lw x8,  32(sp)
    lw x9,  36(sp)
    lw x10, 40(sp)
    lw x11, 44(sp)
    lw x12, 48(sp)
    lw x13, 52(sp)
    lw x14, 56(sp)
    lw x15, 60(sp)
    lw x16, 64(sp)
    lw x17, 68(sp)
    lw x18, 72(sp)
    lw x19, 76(sp)
    lw x20, 80(sp)
    lw x21, 84(sp)
    lw x22, 88(sp)
    lw x23, 92(sp)
    lw x24, 96(sp)
    lw x25, 100(sp)
    lw x26, 104(sp)
    lw x27, 108(sp)
    lw x28, 112(sp)
    lw x29, 116(sp)
    lw x30, 120(sp)
    lw x31, 124(sp)

    /* Restore original sp and mscratch for future traps.
     * Load original sp into mscratch, then swap sp <-> mscratch.
     * This restores sp to the original value and sets mscratch
     * back to the BRAM trap stack top (current sp after dealloc). */
    lw x5,   8(sp)         /* x5 = original sp */
    csrw mscratch, x5      /* mscratch = original sp */
    lw x5,  20(sp)         /* restore original t0 */
    addi sp, sp, 272       /* deallocate trap frame; sp = trap stack top */
    csrrw sp, mscratch, sp /* sp = original sp, mscratch = trap stack top */

    /* Return from trap */
    mret

_fatal_trap:
    /* Call fatal_trap(trap_frame_t *frame) - does not return */
    mv a0, sp
    jal ra, fatal_trap
    j _fatal_trap

/* ========================================
 * Switch to a new runtime stack and jump to entry point.
 * a0 = function pointer, a1 = new stack top
 * Does not return.
 * ======================================== */
.global switch_to_runtime_stack_and_call
switch_to_runtime_stack_and_call:
    mv sp, a1
    jr a0
1:
    j 1b
