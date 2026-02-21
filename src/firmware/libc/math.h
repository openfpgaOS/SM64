/* math.h - redirect to our unified libc.h for bare-metal Pocket */
#ifndef _POCKET_MATH_H
#define _POCKET_MATH_H
#include "libc.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef HUGE_VAL
#define HUGE_VAL __builtin_huge_val()
#endif

#ifndef INFINITY
#define INFINITY __builtin_inff()
#endif

#ifndef NAN
#define NAN __builtin_nanf("")
#endif

#endif
