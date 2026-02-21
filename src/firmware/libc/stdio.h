/* stdio.h - redirect to our unified libc.h for bare-metal Pocket */
#ifndef _POCKET_STDIO_H
#define _POCKET_STDIO_H
#include "libc.h"

/* printf/puts - implemented in pocket/printf_pocket.c */
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list args);
int puts(const char *s);
char *fgets(char *s, int size, FILE *stream);

#endif
