/*
 * PocketSM64 Bootloader
 * Runs from BRAM, initializes system, waits for data slot loading, then jumps to SM64
 * Copies sm64.bin from SDRAM to PSRAM (CRAM0) for execution
 */

#include "terminal.h"
#include "dataslot.h"

#define BOOT_VERBOSE 1
#if BOOT_VERBOSE
#define BOOT_LOG(...) term_printf(__VA_ARGS__)
#else
#define BOOT_LOG(...) do {} while (0)
#endif

/* System registers */
#define SYS_STATUS      (*(volatile unsigned int *)0x40000000)
#define SYS_CYCLE_LO    (*(volatile unsigned int *)0x40000004)
#define SYS_CYCLE_HI    (*(volatile unsigned int *)0x40000008)

/* Data slot IDs (from data.json) */
#define SLOT_SM64_BIN   0
#define SLOT_ASSET_DATA 1

/* Load addresses */
#define SM64_BIN_ADDR   0x10200000  /* SDRAM (bridge loads here) */
#define ASSET_DATA_ADDR 0x11000000  /* SDRAM (bridge loads here) */

/* Maximum sizes to load */
#define SM64_BIN_SIZE   (4 * 1024 * 1024)    /* 4 MB */
#define ASSET_DATA_SIZE (20 * 1024 * 1024)   /* 20 MB */

/* External symbols from linker */
extern char _qbss_start[], _qbss_end[];
extern char _runtime_stack_top[];
extern char _app_copy_src[];    /* Source address (SDRAM LMA) */
extern char _app_copy_dst[];    /* Destination address (PSRAM VMA) */
extern char _app_copy_size[];   /* Size of .text + .data to copy */
/* SM64 entry point (linked in PSRAM) */
extern void sm64_main(void);
extern void switch_to_runtime_stack_and_call(void (*entry)(void), void *stack_top);

/* Note: Trap handling moved to misaligned.c for misaligned access emulation */

/* Quick smoke test for FixedPointMacPlugin custom instructions */
__attribute__((section(".text.boot")))
static int test_fx_instructions(void) {
    int pass = 0, fail = 0;
    int result;

    BOOT_LOG("=== FX INSTRUCTION TEST ===\n");

    /* Test 1: FXMUL 1.0 * 1.0 = 1.0  (0x10000 * 0x10000 -> 0x10000) */
    __asm__ volatile(".insn r 0x0B, 0, 0, %0, %1, %2"
        : "=r"(result) : "r"(0x10000), "r"(0x10000));
    BOOT_LOG("FXMUL 1*1=%x %s\n", result, result == 0x10000 ? "OK" : "FAIL");
    if (result == 0x10000) pass++; else fail++;

    /* Test 2: FXMUL 2.0 * 3.0 = 6.0  (0x20000 * 0x30000 -> 0x60000) */
    __asm__ volatile(".insn r 0x0B, 0, 0, %0, %1, %2"
        : "=r"(result) : "r"(0x20000), "r"(0x30000));
    BOOT_LOG("FXMUL 2*3=%x %s\n", result, result == 0x60000 ? "OK" : "FAIL");
    if (result == 0x60000) pass++; else fail++;

    /* Test 3: FXMUL 0.5 * 0.5 = 0.25  (0x8000 * 0x8000 -> 0x4000) */
    __asm__ volatile(".insn r 0x0B, 0, 0, %0, %1, %2"
        : "=r"(result) : "r"(0x8000), "r"(0x8000));
    BOOT_LOG("FXMUL .5*.5=%x %s\n", result, result == 0x4000 ? "OK" : "FAIL");
    if (result == 0x4000) pass++; else fail++;

    /* Test 4: FXMUL (-1.0) * 2.0 = -2.0  (0xFFFF0000 * 0x20000 -> 0xFFFE0000) */
    __asm__ volatile(".insn r 0x0B, 0, 0, %0, %1, %2"
        : "=r"(result) : "r"((int)0xFFFF0000), "r"(0x20000));
    BOOT_LOG("FXMUL -1*2=%x %s\n", result, result == (int)0xFFFE0000 ? "OK" : "FAIL");
    if (result == (int)0xFFFE0000) pass++; else fail++;

    /* Test 5: FXMACS/FXMACR dot product: 1*2 + 3*4 = 14.0 (0xE0000) */
    /* First clear accumulator by reading it */
    __asm__ volatile(".insn r 0x0B, 2, 0, %0, x0, x0" : "=r"(result));
    /* Now accumulate */
    __asm__ volatile(".insn r 0x0B, 1, 0, x0, %0, %1"
        : : "r"(0x10000), "r"(0x20000));  /* acc += 1*2 */
    __asm__ volatile(".insn r 0x0B, 1, 0, x0, %0, %1"
        : : "r"(0x30000), "r"(0x40000));  /* acc += 3*4 */
    __asm__ volatile(".insn r 0x0B, 2, 0, %0, x0, x0" : "=r"(result));
    BOOT_LOG("MAC 1*2+3*4=%x %s\n", result, result == 0xE0000 ? "OK" : "FAIL");
    if (result == 0xE0000) pass++; else fail++;

    /* Test 6: FXMACR should have cleared acc — verify reads 0 */
    __asm__ volatile(".insn r 0x0B, 2, 0, %0, x0, x0" : "=r"(result));
    BOOT_LOG("MAC clear=%x %s\n", result, result == 0 ? "OK" : "FAIL");
    if (result == 0) pass++; else fail++;

    /* Test 7: FXRCP(1.0) = 1.0  (0x10000 -> ~0x10000) */
    __asm__ volatile(".insn r 0x0B, 3, 0, %0, %1, x0"
        : "=r"(result) : "r"(0x10000));
    /* Accept 0xFFFF or 0x10000 (1 LSB tolerance from LUT saturation) */
    BOOT_LOG("RCP 1.0=%x %s\n", result,
        (result >= 0xFFF0 && result <= 0x10010) ? "OK" : "FAIL");
    if (result >= 0xFFF0 && result <= 0x10010) pass++; else fail++;

    /* Test 8: FXRCP(2.0) = 0.5  (0x20000 -> ~0x8000) */
    __asm__ volatile(".insn r 0x0B, 3, 0, %0, %1, x0"
        : "=r"(result) : "r"(0x20000));
    BOOT_LOG("RCP 2.0=%x %s\n", result,
        (result >= 0x7FF0 && result <= 0x8010) ? "OK" : "FAIL");
    if (result >= 0x7FF0 && result <= 0x8010) pass++; else fail++;

    /* Test 9: FXRCP(0.5) = 2.0  (0x8000 -> ~0x20000) */
    __asm__ volatile(".insn r 0x0B, 3, 0, %0, %1, x0"
        : "=r"(result) : "r"(0x8000));
    BOOT_LOG("RCP 0.5=%x %s\n", result,
        (result >= 0x1FFF0 && result <= 0x20010) ? "OK" : "FAIL");
    if (result >= 0x1FFF0 && result <= 0x20010) pass++; else fail++;

    /* Test 10: FXRCP(-1.0) = -1.0  (0xFFFF0000 -> ~0xFFFF0000) */
    __asm__ volatile(".insn r 0x0B, 3, 0, %0, %1, x0"
        : "=r"(result) : "r"((int)0xFFFF0000));
    BOOT_LOG("RCP -1.0=%x %s\n", result,
        (result <= (int)0xFFFF0010 && result >= (int)0xFFFEFFF0) ? "OK" : "FAIL");
    if (result <= (int)0xFFFF0010 && result >= (int)0xFFFEFFF0) pass++; else fail++;

    /* Test 11: FXRCP(4.0) = 0.25  (0x40000 -> ~0x4000) */
    __asm__ volatile(".insn r 0x0B, 3, 0, %0, %1, x0"
        : "=r"(result) : "r"(0x40000));
    BOOT_LOG("RCP 4.0=%x %s\n", result,
        (result >= 0x3FF0 && result <= 0x4010) ? "OK" : "FAIL");
    if (result >= 0x3FF0 && result <= 0x4010) pass++; else fail++;

    BOOT_LOG("FX: Pass:%d Fail:%d\n", pass, fail);
    return fail;
}

/* Clear BSS section with progress reporting */
__attribute__((section(".text.boot")))
static void clear_qbss(void) {
    unsigned int *p = (unsigned int *)_qbss_start;
    unsigned int *end = (unsigned int *)_qbss_end;
    unsigned int *next_report = p + (64 * 1024 / 4);  /* Report every 64KB */
    int count = 0;

    BOOT_LOG("loop @%x\n", (unsigned int)p);
    while (p < end) {
        *p++ = 0;
        if (p >= next_report) {
            BOOT_LOG("@%x\n", (unsigned int)p);
            next_report += (64 * 1024 / 4);
            count++;
        }
    }
    BOOT_LOG("Done(%d)\n", count);
}

/* Copy app binary from SDRAM (LMA) to PSRAM (VMA) for execution */
__attribute__((section(".text.boot")))
static void copy_to_psram(void) {
    volatile unsigned int *src = (volatile unsigned int *)_app_copy_src;
    volatile unsigned int *dst = (volatile unsigned int *)_app_copy_dst;
    unsigned int words = (unsigned int)_app_copy_size / 4;
    unsigned int i;

    BOOT_LOG("Copy SDRAM 0x%x -> PSRAM 0x%x (%d bytes)\n",
             (unsigned int)src, (unsigned int)dst, (unsigned int)_app_copy_size);

    for (i = 0; i < words; i++)
        dst[i] = src[i];

    /* Fence: flush D-cache dirty lines to PSRAM, then invalidate I-cache
     * so instruction fetches see the freshly-copied code.
     * fence.i = 0x0000100f (raw encoding avoids needing zifencei in -march) */
    __asm__ volatile("fence");
    __asm__ volatile(".word 0x0000100f");  /* fence.i */

    BOOT_LOG("Copy done, fence.i issued\n");
}

__attribute__((section(".text.boot")))
int main(void) {
    /* Initialize terminal early for debug output (safe: uses terminal BRAM) */
    term_init();
    BOOT_LOG("Boot @ 100MHz\n\n");
    BOOT_LOG("Waiting for dataslot_allcomplete (SYS_STATUS bit1)...\n");

    /* CRITICAL: Wait for APF dataslot loading BEFORE touching SDRAM */
    unsigned int last_report = SYS_CYCLE_LO;
    unsigned int start_wait = last_report;
    while (!(SYS_STATUS & (1 << 1))) {
        unsigned int now = SYS_CYCLE_LO;
        if ((now - last_report) > 6600000) {  /* ~0.1s at 66MHz */
            BOOT_LOG("SYS_STATUS=0x%x\n", SYS_STATUS);
            last_report = now;
        }
        if ((now - start_wait) > 240000000) { /* ~5s timeout */
            BOOT_LOG("Timeout waiting for dataslot; continuing anyway.\n");
            break;
        }
    }

    /* Keep boot checks lightweight to avoid triggering timing-sensitive failures. */
    BOOT_LOG("=== SDRAM SMOKE TEST ===\n");
    volatile unsigned int *test = (volatile unsigned int *)0x13000000;
    unsigned int rb;
    int pass_count = 0, fail_count = 0;

    BOOT_LOG("[Test 1: 32-bit W/R]\n");
    test[0] = 0xAABBCCDD;
    rb = test[0];
    BOOT_LOG("W:AABBCCDD R:%x %s\n", rb, rb == 0xAABBCCDD ? "OK" : "FAIL");
    if (rb == 0xAABBCCDD) pass_count++; else fail_count++;

    test[0] = 0x12345678;
    rb = test[0];
    BOOT_LOG("W:12345678 R:%x %s\n", rb, rb == 0x12345678 ? "OK" : "FAIL");
    if (rb == 0x12345678) pass_count++; else fail_count++;

    BOOT_LOG("Pass:%d Fail:%d\n", pass_count, fail_count);

    /* Test custom FX instructions */
    test_fx_instructions();

    /* Show BSS region for debugging */
    BOOT_LOG("BSS: 0x%x - 0x%x\n", (unsigned int)_qbss_start, (unsigned int)_qbss_end);
    BOOT_LOG("BSS size: %d bytes\n", (int)(_qbss_end - _qbss_start));

    /* Copy app code+data from SDRAM to PSRAM */
    BOOT_LOG("\n=== COPY TO PSRAM ===\n");
    copy_to_psram();

    /* Assets stay in cached SDRAM (0x11000000) for fast D-cache burst reads. */
    BOOT_LOG("Assets: cached SDRAM @ 0x11000000\n");

    /* Clear BSS section before running app */
    BOOT_LOG("\nClearing BSS 0x%x-0x%x...\n", (unsigned int)_qbss_start, (unsigned int)_qbss_end);

    /* Test first BSS write before full clear */
    volatile unsigned int *bss_test = (volatile unsigned int *)_qbss_start;
    BOOT_LOG("BSS test write @0x%x...\n", (unsigned int)bss_test);
    *bss_test = 0;
    BOOT_LOG("BSS test write OK\n");

    BOOT_LOG("Calling clear_qbss...\n");
    clear_qbss();
    BOOT_LOG("BSS cleared.\n");

    /* Jump to SM64! */
    BOOT_LOG("\nStarting SM64...\n");
    BOOT_LOG("sm64_main @ 0x%x\n", (unsigned int)sm64_main);
    BOOT_LOG("runtime stack top @ 0x%x\n", (unsigned int)_runtime_stack_top);

    BOOT_LOG("Jumping now...\n");
    switch_to_runtime_stack_and_call(sm64_main, _runtime_stack_top);

    /* Test: if we get here, instruction fetch from PSRAM worked! */
    BOOT_LOG("SUCCESS: sm64_main returned!\n");
    BOOT_LOG("PSRAM instruction fetch works!\n");
    while (1) {}

    return 0;
}
