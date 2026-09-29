//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * printf_pocket.c -- printf/puts routing for PocketSM64
 * Routes formatted output to the terminal display.
 */

#ifdef TARGET_POCKET

#include "../libc/libc.h"
#include "../terminal.h"

/* SM64 pc_main.c does #define printf to suppress output.
 * We #undef it here so our real implementation is visible. */
#undef printf

int printf(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    term_puts(buf);
    return n;
}

int vprintf(const char *fmt, va_list args) {
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    term_puts(buf);
    return n;
}

int puts(const char *s) {
    term_puts(s);
    term_puts("\n");
    return 0;
}

#endif /* TARGET_POCKET */
