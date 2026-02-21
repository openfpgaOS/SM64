/*
 * PocketSM64 System Dashboard
 * SDRAM stress test + CPU instruction verification
 */

#include <stdint.h>
#include "font8x8.h"

/* Hardware registers */
#define SYS_STATUS        (*(volatile uint32_t*)0x40000000)
#define SYS_CYCLE_LO      (*(volatile uint32_t*)0x40000004)
#define SYS_CYCLE_HI      (*(volatile uint32_t*)0x40000008)
#define SYS_DISPLAY_MODE  (*(volatile uint32_t*)0x4000000C)
#define SYS_FB_SWAP       (*(volatile uint32_t*)0x40000018)

/* Framebuffer addresses in SDRAM */
#define FRAMEBUFFER_0     ((volatile uint16_t*)0x10000000)
#define FRAMEBUFFER_1     ((volatile uint16_t*)0x10100000)

/* SDRAM test region (after framebuffers) */
#define SDRAM_TEST_BASE   ((volatile uint32_t*)0x10200000)
#define SDRAM_TEST_SIZE   (1024 * 1024)  /* 1MB test region */

/* PSRAM test region */
#define PSRAM_TEST_BASE   ((volatile uint32_t*)0x30000000)
#define PSRAM_TEST_SIZE   (1024 * 1024)  /* 1MB test region (of 16MB available) */

/* Display constants */
#define FB_WIDTH   320
#define FB_HEIGHT  240

/* Colors - dark theme */
#define COL_BG          0x0841   /* Dark gray background */
#define COL_PANEL       0x1082   /* Panel background */
#define COL_BORDER      0x4A69   /* Panel border */
#define COL_TITLE_BG    0x0010   /* Title bar dark blue */
#define COL_TEXT        0xFFFF   /* White text */
#define COL_TEXT_DIM    0x8410   /* Dim gray text */
#define COL_PASS        0x07E0   /* Green */
#define COL_FAIL        0xF800   /* Red */
#define COL_WARN        0xFD20   /* Orange */
#define COL_PROGRESS_BG 0x2104   /* Progress bar background */
#define COL_PROGRESS    0x04FF   /* Progress bar fill (cyan) */
#define COL_HIGHLIGHT   0xFFE0   /* Yellow highlight */

/* Current draw buffer */
static volatile uint16_t* draw_buffer = FRAMEBUFFER_1;

/* Test results */
static int sdram_errors = 0;
static int sdram_mb_tested = 0;
static int psram_errors = 0;
static int psram_mb_tested = 0;
static int cpu_tests_passed = 0;
static int cpu_tests_total = 0;

/* ============================================ */
/* Graphics primitives                          */
/* ============================================ */

static inline void put_pixel(int x, int y, uint16_t color) {
    if (x >= 0 && x < FB_WIDTH && y >= 0 && y < FB_HEIGHT) {
        draw_buffer[y * FB_WIDTH + x] = color;
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            put_pixel(x + i, y + j, color);
        }
    }
}

static void draw_rect(int x, int y, int w, int h, uint16_t color) {
    for (int i = 0; i < w; i++) {
        put_pixel(x + i, y, color);
        put_pixel(x + i, y + h - 1, color);
    }
    for (int j = 0; j < h; j++) {
        put_pixel(x, y + j, color);
        put_pixel(x + w - 1, y + j, color);
    }
}

static void draw_char(int x, int y, char c, uint16_t color) {
    if (c < 32 || c > 127) c = '?';
    const uint8_t* glyph = font8x8[c - 32];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                put_pixel(x + col, y + row, color);
            }
        }
    }
}

static void draw_string(int x, int y, const char* str, uint16_t color) {
    while (*str) {
        draw_char(x, y, *str++, color);
        x += 8;
    }
}

static void draw_string_center(int y, const char* str, uint16_t color) {
    int len = 0;
    const char* p = str;
    while (*p++) len++;
    int x = (FB_WIDTH - len * 8) / 2;
    draw_string(x, y, str, color);
}

/* Draw a number (right-aligned) */
static void draw_number(int x, int y, uint32_t num, int digits, uint16_t color) {
    char buf[12];
    int i = 11;
    buf[i--] = '\0';
    if (num == 0) {
        buf[i--] = '0';
    } else {
        while (num > 0 && i >= 0) {
            buf[i--] = '0' + (num % 10);
            num /= 10;
        }
    }
    /* Pad with spaces */
    while (i >= 12 - digits - 1 && i >= 0) {
        buf[i--] = ' ';
    }
    draw_string(x, y, &buf[i + 1], color);
}

static void draw_hex(int x, int y, uint32_t num, int digits, uint16_t color) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) {
        draw_char(x + i * 8, y, hex[num & 0xF], color);
        num >>= 4;
    }
}

static void draw_progress_bar(int x, int y, int w, int h, int percent, uint16_t fg, uint16_t bg) {
    fill_rect(x, y, w, h, bg);
    int fill_w = (w * percent) / 100;
    if (fill_w > 0) {
        fill_rect(x, y, fill_w, h, fg);
    }
    draw_rect(x, y, w, h, COL_BORDER);
}

/* ============================================ */
/* UI Panel drawing                             */
/* ============================================ */

static void draw_panel(int x, int y, int w, int h, const char* title) {
    /* Panel background */
    fill_rect(x, y, w, h, COL_PANEL);

    /* Title bar */
    fill_rect(x, y, w, 12, COL_TITLE_BG);
    draw_string(x + 4, y + 2, title, COL_TEXT);

    /* Border */
    draw_rect(x, y, w, h, COL_BORDER);
}

/* ============================================ */
/* SDRAM Stress Test                            */
/* ============================================ */

static int test_sdram_pattern(uint32_t pattern, int offset, int count) {
    volatile uint32_t* base = SDRAM_TEST_BASE + offset;
    int errors = 0;

    /* Write pattern */
    for (int i = 0; i < count; i++) {
        base[i] = pattern;
    }

    /* Read and verify */
    for (int i = 0; i < count; i++) {
        uint32_t val = base[i];
        if (val != pattern) {
            errors++;
        }
    }

    return errors;
}

static int test_sdram_walking(int offset, int count) {
    volatile uint32_t* base = SDRAM_TEST_BASE + offset;
    int errors = 0;

    /* Walking ones */
    for (int i = 0; i < count && i < 32; i++) {
        uint32_t pattern = 1 << i;
        base[i] = pattern;
    }
    for (int i = 0; i < count && i < 32; i++) {
        uint32_t expected = 1 << i;
        if (base[i] != expected) errors++;
    }

    /* Walking zeros */
    for (int i = 0; i < count && i < 32; i++) {
        uint32_t pattern = ~(1 << i);
        base[i] = pattern;
    }
    for (int i = 0; i < count && i < 32; i++) {
        uint32_t expected = ~(1 << i);
        if (base[i] != expected) errors++;
    }

    return errors;
}

static int test_sdram_address(int offset, int count) {
    volatile uint32_t* base = SDRAM_TEST_BASE + offset;
    int errors = 0;

    /* Write address as data */
    for (int i = 0; i < count; i++) {
        base[i] = (uint32_t)(uintptr_t)&base[i];
    }

    /* Verify */
    for (int i = 0; i < count; i++) {
        uint32_t expected = (uint32_t)(uintptr_t)&base[i];
        if (base[i] != expected) errors++;
    }

    return errors;
}

/* ============================================ */
/* PSRAM Stress Test                            */
/* ============================================ */

static int test_psram_pattern(uint32_t pattern, int offset, int count) {
    volatile uint32_t* base = PSRAM_TEST_BASE + offset;
    int errors = 0;

    /* Write pattern */
    for (int i = 0; i < count; i++) {
        base[i] = pattern;
    }

    /* Read and verify */
    for (int i = 0; i < count; i++) {
        uint32_t val = base[i];
        if (val != pattern) {
            errors++;
        }
    }

    return errors;
}

static int test_psram_walking(int offset, int count) {
    volatile uint32_t* base = PSRAM_TEST_BASE + offset;
    int errors = 0;

    /* Walking ones */
    for (int i = 0; i < count && i < 32; i++) {
        uint32_t pattern = 1 << i;
        base[i] = pattern;
    }
    for (int i = 0; i < count && i < 32; i++) {
        uint32_t expected = 1 << i;
        if (base[i] != expected) errors++;
    }

    /* Walking zeros */
    for (int i = 0; i < count && i < 32; i++) {
        uint32_t pattern = ~(1 << i);
        base[i] = pattern;
    }
    for (int i = 0; i < count && i < 32; i++) {
        uint32_t expected = ~(1 << i);
        if (base[i] != expected) errors++;
    }

    return errors;
}

static int test_psram_address(int offset, int count) {
    volatile uint32_t* base = PSRAM_TEST_BASE + offset;
    int errors = 0;

    /* Write address as data */
    for (int i = 0; i < count; i++) {
        base[i] = (uint32_t)(uintptr_t)&base[i];
    }

    /* Verify */
    for (int i = 0; i < count; i++) {
        uint32_t expected = (uint32_t)(uintptr_t)&base[i];
        if (base[i] != expected) errors++;
    }

    return errors;
}

/* ============================================ */
/* CPU Instruction Tests                        */
/* ============================================ */

#define TEST(name, expr) do { \
    cpu_tests_total++; \
    if (expr) { cpu_tests_passed++; } \
} while(0)

static void test_cpu_arithmetic(void) {
    volatile int a = 100, b = 25;

    TEST("ADD", a + b == 125);
    TEST("SUB", a - b == 75);
    TEST("MUL", a * b == 2500);
    TEST("DIV", a / b == 4);
    TEST("REM", a % b == 0);
    TEST("NEG", -a == -100);
}

static void test_cpu_logical(void) {
    volatile uint32_t a = 0xFF00FF00, b = 0x0F0F0F0F;

    TEST("AND", (a & b) == 0x0F000F00);
    TEST("OR",  (a | b) == 0xFF0FFF0F);
    TEST("XOR", (a ^ b) == 0xF00FF00F);
    TEST("NOT", (~a) == 0x00FF00FF);
}

static void test_cpu_shifts(void) {
    volatile uint32_t a = 0x80000001;
    volatile int32_t sa = -16;

    TEST("SLL", (a << 4) == 0x00000010);
    TEST("SRL", (a >> 4) == 0x08000000);
    TEST("SRA", (sa >> 2) == -4);  /* Arithmetic shift */
}

static void test_cpu_compare(void) {
    volatile int a = -5, b = 10;
    volatile uint32_t ua = 0xFFFFFFFF, ub = 1;

    TEST("SLT", (a < b) == 1);
    TEST("SGE", (b >= a) == 1);
    TEST("SLTU", (ub < ua) == 1);  /* Unsigned compare */
}

static void test_cpu_memory(void) {
    volatile uint32_t val32 = 0xDEADBEEF;
    volatile uint16_t val16 = 0xCAFE;
    volatile uint8_t val8 = 0x42;

    uint32_t r32 = val32;
    uint16_t r16 = val16;
    uint8_t r8 = val8;

    TEST("LW/SW", r32 == 0xDEADBEEF);
    TEST("LH/SH", r16 == 0xCAFE);
    TEST("LB/SB", r8 == 0x42);
}

static void test_cpu_branch(void) {
    volatile int x = 0;
    volatile int a = 5, b = 5, c = 10;

    if (a == b) x = 1;
    TEST("BEQ", x == 1);

    x = 0;
    if (a != c) x = 1;
    TEST("BNE", x == 1);

    x = 0;
    if (a < c) x = 1;
    TEST("BLT", x == 1);

    x = 0;
    if (c >= a) x = 1;
    TEST("BGE", x == 1);
}

/* ============================================ */
/* Buffer swap                                  */
/* ============================================ */

static void swap_buffers(void) {
    SYS_FB_SWAP = 1;
    while (SYS_FB_SWAP & 1);
    draw_buffer = (draw_buffer == FRAMEBUFFER_1) ? FRAMEBUFFER_0 : FRAMEBUFFER_1;
}

/* ============================================ */
/* Main dashboard                               */
/* ============================================ */

static void draw_dashboard(int sdram_progress, int psram_progress, uint32_t cycles) {
    /* Clear screen */
    fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, COL_BG);

    /* Title */
    fill_rect(0, 0, FB_WIDTH, 14, COL_TITLE_BG);
    draw_string_center(3, "PocketSM64 System Dashboard", COL_HIGHLIGHT);

    /* System info panel */
    draw_panel(5, 18, 150, 38, "System Info");
    draw_string(10, 32, "CPU:", COL_TEXT_DIM);
    draw_string(42, 32, "VexRiscv 133MHz", COL_TEXT);
    draw_string(10, 42, "SDRAM:", COL_TEXT_DIM);
    draw_string(58, 42, "64MB", COL_TEXT);
    draw_string(95, 42, "PSRAM:", COL_TEXT_DIM);
    draw_string(143, 42, "16MB", COL_TEXT);

    /* Cycle counter panel */
    draw_panel(165, 18, 150, 38, "Cycle Counter");
    draw_string(170, 36, "Cycles:", COL_TEXT_DIM);
    draw_hex(230, 36, cycles >> 16, 4, COL_TEXT);
    draw_hex(262, 36, cycles & 0xFFFF, 4, COL_TEXT);

    /* SDRAM Test panel */
    draw_panel(5, 60, 155, 58, "SDRAM Test");

    draw_string(10, 74, "Prog:", COL_TEXT_DIM);
    draw_progress_bar(48, 73, 80, 10, sdram_progress, COL_PROGRESS, COL_PROGRESS_BG);
    char pct[8];
    pct[0] = '0' + (sdram_progress / 100) % 10;
    pct[1] = '0' + (sdram_progress / 10) % 10;
    pct[2] = '0' + sdram_progress % 10;
    pct[3] = '%';
    pct[4] = '\0';
    draw_string(132, 74, pct, COL_TEXT);

    draw_string(10, 86, "KB:", COL_TEXT_DIM);
    draw_number(32, 86, sdram_mb_tested, 4, COL_TEXT);
    draw_string(80, 86, "Err:", COL_TEXT_DIM);
    draw_number(108, 86, sdram_errors, 4, sdram_errors == 0 ? COL_PASS : COL_FAIL);

    draw_string(10, 100, "Status:", COL_TEXT_DIM);
    if (sdram_progress < 100) {
        draw_string(62, 100, "Testing...", COL_WARN);
    } else if (sdram_errors == 0) {
        draw_string(62, 100, "PASSED", COL_PASS);
    } else {
        draw_string(62, 100, "FAILED", COL_FAIL);
    }

    /* PSRAM Test panel */
    draw_panel(165, 60, 150, 58, "PSRAM Test");

    draw_string(170, 74, "Prog:", COL_TEXT_DIM);
    draw_progress_bar(208, 73, 80, 10, psram_progress, COL_PROGRESS, COL_PROGRESS_BG);
    char pct2[8];
    pct2[0] = '0' + (psram_progress / 100) % 10;
    pct2[1] = '0' + (psram_progress / 10) % 10;
    pct2[2] = '0' + psram_progress % 10;
    pct2[3] = '%';
    pct2[4] = '\0';
    draw_string(292, 74, pct2, COL_TEXT);

    draw_string(170, 86, "KB:", COL_TEXT_DIM);
    draw_number(192, 86, psram_mb_tested, 4, COL_TEXT);
    draw_string(240, 86, "Err:", COL_TEXT_DIM);
    draw_number(268, 86, psram_errors, 4, psram_errors == 0 ? COL_PASS : COL_FAIL);

    draw_string(170, 100, "Status:", COL_TEXT_DIM);
    if (psram_progress < 100) {
        draw_string(222, 100, "Testing...", COL_WARN);
    } else if (psram_errors == 0) {
        draw_string(222, 100, "PASSED", COL_PASS);
    } else {
        draw_string(222, 100, "FAILED", COL_FAIL);
    }

    /* CPU Test panel */
    draw_panel(5, 122, 310, 115, "CPU Instruction Tests");

    draw_string(10, 136, "Arithmetic:", COL_TEXT_DIM);
    draw_string(96, 136, "ADD SUB MUL DIV REM NEG", COL_TEXT);
    draw_string(10, 148, "Logical:", COL_TEXT_DIM);
    draw_string(80, 148, "AND OR XOR NOT", COL_TEXT);
    draw_string(10, 160, "Shifts:", COL_TEXT_DIM);
    draw_string(72, 160, "SLL SRL SRA", COL_TEXT);
    draw_string(10, 172, "Compare:", COL_TEXT_DIM);
    draw_string(80, 172, "SLT SGE SLTU", COL_TEXT);
    draw_string(10, 184, "Memory:", COL_TEXT_DIM);
    draw_string(72, 184, "LW/SW LH/SH LB/SB", COL_TEXT);
    draw_string(10, 196, "Branch:", COL_TEXT_DIM);
    draw_string(72, 196, "BEQ BNE BLT BGE", COL_TEXT);

    /* Results */
    draw_string(10, 218, "Total:", COL_TEXT_DIM);
    draw_number(60, 218, cpu_tests_passed, 2, COL_TEXT);
    draw_string(80, 218, "/", COL_TEXT);
    draw_number(90, 218, cpu_tests_total, 2, COL_TEXT);

    if (cpu_tests_total > 0) {
        if (cpu_tests_passed == cpu_tests_total) {
            draw_string(130, 218, "ALL PASS", COL_PASS);
        } else {
            draw_string(130, 218, "FAILED", COL_FAIL);
        }
    }
}

int main(void) {
    /* Switch to framebuffer mode */
    SYS_DISPLAY_MODE = 1;

    /* Run CPU tests first */
    test_cpu_arithmetic();
    test_cpu_logical();
    test_cpu_shifts();
    test_cpu_compare();
    test_cpu_memory();
    test_cpu_branch();

    /* Main loop - run SDRAM and PSRAM tests */
    int sdram_test_phase = 0;
    int sdram_test_offset = 0;
    int psram_test_phase = 0;
    int psram_test_offset = 0;
    int words_per_iter = 1024;  /* Test 4KB per iteration */
    int sdram_total_words = SDRAM_TEST_SIZE / 4;
    int psram_total_words = PSRAM_TEST_SIZE / 4;

    while (1) {
        uint32_t cycles = SYS_CYCLE_LO;

        /* Calculate progress */
        int sdram_progress = (sdram_test_offset * 100) / sdram_total_words;
        if (sdram_progress > 100) sdram_progress = 100;
        int psram_progress = (psram_test_offset * 100) / psram_total_words;
        if (psram_progress > 100) psram_progress = 100;

        /* Draw dashboard */
        draw_dashboard(sdram_progress, psram_progress, cycles);
        swap_buffers();

        /* Run SDRAM tests if not complete */
        if (sdram_test_offset < sdram_total_words) {
            int count = words_per_iter;
            if (sdram_test_offset + count > sdram_total_words) {
                count = sdram_total_words - sdram_test_offset;
            }

            switch (sdram_test_phase) {
                case 0:
                    sdram_errors += test_sdram_pattern(0xAAAAAAAA, sdram_test_offset, count);
                    break;
                case 1:
                    sdram_errors += test_sdram_pattern(0x55555555, sdram_test_offset, count);
                    break;
                case 2:
                    sdram_errors += test_sdram_pattern(0xFFFFFFFF, sdram_test_offset, count);
                    break;
                case 3:
                    sdram_errors += test_sdram_pattern(0x00000000, sdram_test_offset, count);
                    break;
                case 4:
                    sdram_errors += test_sdram_walking(sdram_test_offset, count);
                    break;
                case 5:
                    sdram_errors += test_sdram_address(sdram_test_offset, count);
                    sdram_test_offset += count;
                    sdram_mb_tested = (sdram_test_offset * 4) / 1024;
                    break;
            }

            sdram_test_phase = (sdram_test_phase + 1) % 6;
        }

        /* Run PSRAM tests if not complete */
        if (psram_test_offset < psram_total_words) {
            int count = words_per_iter;
            if (psram_test_offset + count > psram_total_words) {
                count = psram_total_words - psram_test_offset;
            }

            switch (psram_test_phase) {
                case 0:
                    psram_errors += test_psram_pattern(0xAAAAAAAA, psram_test_offset, count);
                    break;
                case 1:
                    psram_errors += test_psram_pattern(0x55555555, psram_test_offset, count);
                    break;
                case 2:
                    psram_errors += test_psram_pattern(0xFFFFFFFF, psram_test_offset, count);
                    break;
                case 3:
                    psram_errors += test_psram_pattern(0x00000000, psram_test_offset, count);
                    break;
                case 4:
                    psram_errors += test_psram_walking(psram_test_offset, count);
                    break;
                case 5:
                    psram_errors += test_psram_address(psram_test_offset, count);
                    psram_test_offset += count;
                    psram_mb_tested = (psram_test_offset * 4) / 1024;
                    break;
            }

            psram_test_phase = (psram_test_phase + 1) % 6;
        }
    }

    return 0;
}
