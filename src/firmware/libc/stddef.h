/* stddef.h - provide size_t, ptrdiff_t, NULL for bare-metal Pocket.
 * Uses GCC built-in type names so these match the target ABI.
 * This must be found BEFORE sm64/include/libc/stddef.h in the -I path. */
#ifndef _POCKET_STDDEF_H
#define _POCKET_STDDEF_H

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#endif
