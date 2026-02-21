/*
 * printf/vprintf implementations for PocketQuake
 * Routes to terminal output
 */

#include "../libc/libc.h"
#include "../terminal.h"

#undef printf

int printf(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    term_puts(buf);
    return n;
}

int vprintf(const char *fmt, va_list args) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    term_puts(buf);
    return n;
}
