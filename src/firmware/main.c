/*
 * PocketSM64 Bootloader
 * Runs from BRAM, loads sm64.bin via deferload into SDRAM, then jumps to SM64
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

/* External symbols from linker */
extern char _qbss_start[], _qbss_end[];
extern char _runtime_stack_top[];
extern char _app_load_addr[];  /* SDRAM address where sm64.bin is loaded */
extern char _app_load_size[];  /* Size to load */
/* SM64 entry point (linked in SDRAM) */
extern void sm64_main(void);
extern void switch_to_runtime_stack_and_call(void (*entry)(void), void *stack_top);

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
    __asm__ volatile(".insn r 0x0B, 2, 0, %0, x0, x0" : "=r"(result));
    __asm__ volatile(".insn r 0x0B, 1, 0, x0, %0, %1"
        : : "r"(0x10000), "r"(0x20000));
    __asm__ volatile(".insn r 0x0B, 1, 0, x0, %0, %1"
        : : "r"(0x30000), "r"(0x40000));
    __asm__ volatile(".insn r 0x0B, 2, 0, %0, x0, x0" : "=r"(result));
    BOOT_LOG("MAC 1*2+3*4=%x %s\n", result, result == 0xE0000 ? "OK" : "FAIL");
    if (result == 0xE0000) pass++; else fail++;

    /* Test 6: FXMACR should have cleared acc */
    __asm__ volatile(".insn r 0x0B, 2, 0, %0, x0, x0" : "=r"(result));
    BOOT_LOG("MAC clear=%x %s\n", result, result == 0 ? "OK" : "FAIL");
    if (result == 0) pass++; else fail++;

    /* Test 7: FXRCP(1.0) = 1.0 */
    __asm__ volatile(".insn r 0x0B, 3, 0, %0, %1, x0"
        : "=r"(result) : "r"(0x10000));
    BOOT_LOG("RCP 1.0=%x %s\n", result,
        (result >= 0xFFF0 && result <= 0x10010) ? "OK" : "FAIL");
    if (result >= 0xFFF0 && result <= 0x10010) pass++; else fail++;

    /* Test 8: FXRCP(2.0) = 0.5 */
    __asm__ volatile(".insn r 0x0B, 3, 0, %0, %1, x0"
        : "=r"(result) : "r"(0x20000));
    BOOT_LOG("RCP 2.0=%x %s\n", result,
        (result >= 0x7FF0 && result <= 0x8010) ? "OK" : "FAIL");
    if (result >= 0x7FF0 && result <= 0x8010) pass++; else fail++;

    BOOT_LOG("FX: Pass:%d Fail:%d\n", pass, fail);
    return fail;
}

/* Load sm64.bin from data slot 0 into SDRAM via deferload */
__attribute__((section(".text.boot")))
static int load_sm64_bin_from_slot(void) {
    uint32_t total = (uint32_t)_app_load_size;
    uint32_t base = (uint32_t)_app_load_addr;
    uint32_t done = 0;

    BOOT_LOG("Loading sm64.bin: %d bytes to 0x%x\n", total, base);

    while (done < total) {
        uint32_t chunk = total - done;
        if (chunk > DMA_CHUNK_SIZE)
            chunk = DMA_CHUNK_SIZE;

        int rc = dataslot_read(SLOT_SM64_BIN, done, (void *)(base + done), chunk);
        if (rc < 0) {
            BOOT_LOG("Load FAIL at offset %x: %d\n", done, rc);
            return rc;
        }

        done += chunk;
        if ((done & 0xFFFFF) == 0)  /* Progress every 1MB */
            BOOT_LOG("  %dK / %dK\n", done >> 10, total >> 10);
    }

    BOOT_LOG("Load complete: %d bytes\n", done);
    return 0;
}

/* Clear BSS section */
__attribute__((section(".text.boot")))
static void clear_qbss(void) {
    unsigned int *p = (unsigned int *)_qbss_start;
    unsigned int *end = (unsigned int *)_qbss_end;

    BOOT_LOG("Clearing BSS 0x%x-0x%x...\n", (unsigned int)p, (unsigned int)end);
    while (p < end)
        *p++ = 0;
    BOOT_LOG("BSS cleared.\n");
}

__attribute__((section(".text.boot")))
int main(void) {
    term_init();
    BOOT_LOG("PocketSM64 boot\n");

    /* Wait for APF bridge allcomplete before issuing dataslot commands */
    BOOT_LOG("Waiting for allcomplete...\n");
    unsigned int start_wait = SYS_CYCLE_LO;
    while (!(SYS_STATUS & (1 << 1))) {
        if ((SYS_CYCLE_LO - start_wait) > 500000000) { /* 5s timeout */
            BOOT_LOG("Timeout; continuing.\n");
            break;
        }
    }

    /* Test custom FX instructions */
    test_fx_instructions();

    /* Light SDRAM smoke test */
    BOOT_LOG("=== SDRAM SMOKE TEST ===\n");
    volatile unsigned int *test = (volatile unsigned int *)0x13000000;
    test[0] = 0xAABBCCDD;
    unsigned int rb = test[0];
    BOOT_LOG("W:AABBCCDD R:%x %s\n", rb, rb == 0xAABBCCDD ? "OK" : "FAIL");

    /* Load sm64.bin from SD card via deferload */
    BOOT_LOG("\n=== LOADING SM64 ===\n");
    int rc = load_sm64_bin_from_slot();
    if (rc < 0) {
        BOOT_LOG("FATAL: load failed (%d)\n", rc);
        while (1) {}
    }

    /* Invalidate I-cache so instruction fetches see the loaded code */
    __asm__ volatile("fence");
    __asm__ volatile(".word 0x0000100f");  /* fence.i */

    /* Clear BSS section */
    clear_qbss();

    /* Jump to SM64 */
    BOOT_LOG("\nStarting SM64...\n");
    BOOT_LOG("sm64_main @ 0x%x\n", (unsigned int)sm64_main);
    BOOT_LOG("stack @ 0x%x\n", (unsigned int)_runtime_stack_top);

    switch_to_runtime_stack_and_call(sm64_main, _runtime_stack_top);

    BOOT_LOG("sm64_main returned!\n");
    while (1) {}
    return 0;
}
