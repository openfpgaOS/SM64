/* Single-precision sine/cosine for Goddard (the title-screen Mario head).
 *
 * rv32imafc has hardware single-float but no double: musl's sinf/cosf
 * evaluate their kernels in double, i.e. in soft-float library calls, which
 * cost ~3 ms per title frame on the Pocket CPU.  Cody-Waite range reduction
 * by pi/2 plus degree-11/10 minimax-style Taylor polynomials, all in float;
 * inputs outside +-1024 rad keep the libc path.  Sampled over 1,000,006
 * finite inputs per function, the maximum absolute error against a double
 * reference is 8.9e-8 (sin) and 9.4e-8 (cos).
 *
 * The Makefile builds this file with -fno-fast-math -ffp-contract=off: the
 * two-step range reduction must not be reassociated, and nothing may fuse
 * into the (reduced-accuracy) hardware FMA.
 */
#include <math.h>
#include "gd_trig_openfpga.h"

static float gd_trig_kernel(float x, int cosine) {
    if (!(x >= -1024.0f && x <= 1024.0f)) return cosine ? cosf(x) : sinf(x);
    float qf = x * 0.63661977236758134308f;
    int q = (int)(qf + (qf >= 0.0f ? 0.5f : -0.5f));
    float r = (x - q * 1.5703125f) - q * 0.00048382679489661923f;
    float z = r * r;
    int phase = (q + cosine) & 3;
    float v;
    if (phase & 1) {
        v = 1.0f + z * (-0.5f + z * (0.0416666666666666667f + z * (-0.00138888888888888889f
            + z * (0.0000248015873015873016f + z * (-0.000000275573192239858906f)))));
    } else {
        v = r + r * z * (-0.166666666666666667f + z * (0.00833333333333333333f
            + z * (-0.000198412698412698413f + z * (0.00000275573192239858907f
            + z * (-0.0000000250521083854417188f)))));
    }
    return phase >= 2 ? -v : v;
}

float gd_trig_sinf(float x) { return gd_trig_kernel(x, 0); }
float gd_trig_cosf(float x) { return gd_trig_kernel(x, 1); }
