/*
 * setjmp / longjmp for RV32IM (ilp32 ABI - soft-float)
 * jmp_buf layout (14 x 32-bit words):
 *   [0]  ra   (return address)
 *   [1]  sp   (stack pointer)
 *   [2]  s0   (frame pointer)
 *   [3]  s1
 *   [4]  s2
 *   [5]  s3
 *   [6]  s4
 *   [7]  s5
 *   [8]  s6
 *   [9]  s7
 *   [10] s8
 *   [11] s9
 *   [12] s10
 *   [13] s11
 */

.section .text
.global setjmp
.global longjmp

/* int setjmp(jmp_buf env) */
/* a0 = pointer to jmp_buf */
setjmp:
    sw ra,   0(a0)
    sw sp,   4(a0)
    sw s0,   8(a0)
    sw s1,  12(a0)
    sw s2,  16(a0)
    sw s3,  20(a0)
    sw s4,  24(a0)
    sw s5,  28(a0)
    sw s6,  32(a0)
    sw s7,  36(a0)
    sw s8,  40(a0)
    sw s9,  44(a0)
    sw s10, 48(a0)
    sw s11, 52(a0)
    li a0, 0         /* return 0 from setjmp */
    ret

/* void longjmp(jmp_buf env, int val) */
/* a0 = pointer to jmp_buf, a1 = return value */
longjmp:
    lw ra,   0(a0)
    lw sp,   4(a0)
    lw s0,   8(a0)
    lw s1,  12(a0)
    lw s2,  16(a0)
    lw s3,  20(a0)
    lw s4,  24(a0)
    lw s5,  28(a0)
    lw s6,  32(a0)
    lw s7,  36(a0)
    lw s8,  40(a0)
    lw s9,  44(a0)
    lw s10, 48(a0)
    lw s11, 52(a0)
    mv a0, a1        /* return val */
    bnez a0, 1f
    li a0, 1         /* if val == 0, return 1 */
1:
    ret
