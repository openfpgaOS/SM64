/* assert.h - minimal assert for bare-metal Pocket */
#ifndef _POCKET_ASSERT_H
#define _POCKET_ASSERT_H

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
extern void abort(void) __attribute__((noreturn));
#define assert(expr) ((expr) ? (void)0 : abort())
#endif

#endif
