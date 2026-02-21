/*
 * Simple terminal driver for VexRiscv
 * 40x30 character display at 0x20000000 (320x240 @ 8x8 font)
 */

#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>
#include <stdarg.h>

// Terminal dimensions
#define TERM_COLS 40
#define TERM_ROWS 30
#define TERM_SIZE (TERM_COLS * TERM_ROWS)

// Initialize terminal (clear screen, reset cursor)
void term_init(void);

// Clear the screen
void term_clear(void);

// Set cursor position
void term_setpos(int row, int col);

// Get current position
int term_getpos(void);

// Write a single character (handles newline, wrapping)
void term_putchar(char c);

// Write a string
void term_puts(const char *s);

// Write a string with newline
void term_println(const char *s);

// Printf-style formatted output
// Supports: %d, %u, %x, %X, %s, %c, %%, and width specifiers for hex (e.g., %08X)
void term_printf(const char *fmt, ...);

// Write hex number (useful for debugging)
void term_puthex(uint32_t val, int digits);

// Write decimal number
void term_putdec(int32_t val);

// Convenience: allow using printf() syntax (maps to term_printf)
#define printf term_printf

#endif
