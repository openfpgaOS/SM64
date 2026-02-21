/*
 * ABI-compatible wrappers for __extendsfdf2 and __truncdfsf2.
 *
 * We link against rv32im/ilp32/libgcc.a (no compressed instructions) for
 * most soft-float functions.  However, __extendsfdf2 (float→double) and
 * __truncdfsf2 (double→float) are ABI-incompatible: under ilp32f the float
 * argument/return lives in an FPU register (fa0), whereas in ilp32 it's in
 * an integer register (a0).
 *
 * These implementations use only integer bit manipulation, so the compiler
 * won't generate recursive calls back into __extendsfdf2/__truncdfsf2.
 * Compiled with -march=rv32imaf -mabi=ilp32f, they accept/return float in
 * FPU registers as the calling code expects.
 */

typedef unsigned int      u32;
typedef unsigned long long u64;
typedef union { float f; u32 u; } fu;
typedef union { double d; u64 u; } du;

double __extendsfdf2(float a)
{
    fu in;
    in.f = a;
    u32 bits = in.u;

    u32 sign = bits >> 31;
    int exp  = (int)((bits >> 23) & 0xFF);
    u32 frac = bits & 0x7FFFFF;

    du out;

    if (exp == 0xFF) {
        /* Inf / NaN — preserve payload */
        out.u = ((u64)sign << 63) | ((u64)0x7FF << 52) | ((u64)frac << 29);
    } else if (exp == 0) {
        if (frac == 0) {
            /* +-Zero */
            out.u = (u64)sign << 63;
        } else {
            /* Denormal → normalise */
            int shift = 0;
            u32 tmp = frac;
            while (!(tmp & 0x400000)) { tmp <<= 1; shift++; }
            frac = (tmp << 1) & 0x7FFFFF;
            exp  = 1 - shift;
            out.u = ((u64)sign << 63)
                  | ((u64)(exp + 896) << 52)
                  | ((u64)frac << 29);
        }
    } else {
        /* Normal */
        out.u = ((u64)sign << 63)
              | ((u64)(exp + 896) << 52)
              | ((u64)frac << 29);
    }

    return out.d;
}

float __truncdfsf2(double a)
{
    du in;
    in.d = a;
    u64 bits = in.u;

    u32 sign = (u32)(bits >> 63);
    int exp  = (int)((bits >> 52) & 0x7FF);
    u64 frac = bits & 0x000FFFFFFFFFFFFFULL;

    fu out;

    if (exp == 0x7FF) {
        /* Inf / NaN */
        u32 sfrac = (u32)(frac >> 29);
        if (frac && !sfrac) sfrac = 1; /* preserve NaN-ness */
        out.u = (sign << 31) | (0xFF << 23) | sfrac;
    } else if (exp == 0) {
        /* Zero or double-denormal → float zero (too small) */
        out.u = sign << 31;
    } else {
        int sexp = exp - 896; /* 1023 - 127 */

        if (sexp >= 0xFF) {
            /* Overflow → Inf */
            out.u = (sign << 31) | (0xFF << 23);
        } else if (sexp <= 0) {
            /* Underflow → float denormal or zero */
            int shift = 1 - sexp;
            if (shift > 24) {
                out.u = sign << 31;
            } else {
                /* Add implicit bit and shift right */
                u32 mant = (u32)(frac >> 29) | 0x800000;
                /* Round-to-nearest-even */
                u32 round_bit = 1U << (shift - 1);
                u32 sticky = (mant & (round_bit - 1)) ? 1 : 0;
                mant >>= shift;
                if ((mant & 1) || sticky) {
                    mant += (round_bit >> (shift)) ? 1 : 0;
                }
                /* Simplified: just truncate for denormals */
                mant = ((u32)(frac >> 29) | 0x800000) >> shift;
                out.u = (sign << 31) | mant;
            }
        } else {
            /* Normal: truncate mantissa 52→23 bits with rounding */
            u32 sfrac = (u32)(frac >> 29);
            u32 round = (u32)(frac >> 28) & 1;
            u32 sticky = (frac & 0x0FFFFFFF) ? 1 : 0;

            if (round && (sticky || (sfrac & 1))) {
                sfrac++;
                if (sfrac >= 0x800000) {
                    sfrac = 0;
                    sexp++;
                    if (sexp >= 0xFF) {
                        out.u = (sign << 31) | (0xFF << 23);
                        return out.f;
                    }
                }
            }
            out.u = (sign << 31) | ((u32)sexp << 23) | sfrac;
        }
    }

    return out.f;
}
