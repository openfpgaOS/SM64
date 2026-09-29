/* Exhaustive proof that object_helpers.c's float/double drag, buoyancy and
 * wall-radius math matches the long double (quad on RV32) code it replaced,
 * for every 32-bit float input and every drag strength the game sets
 * (behavior physics 0/100/1000 scaled by 0.01f, Bowser 0/10, particles 0/30).
 *
 * "orig" is what the quad build computed, read from its disassembly:
 *   apply_drag_to_value:  sq = v*v; decel = (float)((quad(sq)*quad(ds)) * Q_1e4),
 *                         then compares against quad 0.001 / -0.001
 *   underwater decelY:    (float)(quad(A) * Q_0_01)  (-ffast-math reciprocal)
 *   wall radius:          radius > Q_0_1
 * with the exact 128-bit constants that build loaded.
 *
 * Not part of check_sm64_optimization.py: it must be built without -ffast-math
 * and takes about 3 minutes on 16 threads.
 *   cc -O2 -ffp-contract=off -msse2 -mfpmath=sse -pthread tools/tests/sm64_quad_math.c -o qx && ./qx
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef __float128 q;

static q qbits(uint64_t hi, uint64_t lo) {
    q r; uint64_t w[2] = { lo, hi }; memcpy(&r, w, 16); return r;
}
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t ubits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static q Q_1e4, Q_1e3, Q_0_01, Q_0_1;
static const double K_1e4 = (double) 1 / 10000;
static const double K_0_01 = (double) 1 / 100;

static float orig_drag(float v, float ds) {
    if (v != 0) {
        float sq = v * v;
        float decel = (float) (((q) sq * (q) ds) * Q_1e4);
        if (v > 0) { v -= decel; if ((q) v < Q_1e3) v = 0; }
        else       { v += decel; if ((q) v > -Q_1e3) v = 0; }
    }
    return v;
}

/* Both association orders -ffast-math may emit for the double form. */
static float new_drag(float v, float ds, int assoc) {
    if (v != 0) {
        float sq = v * v;
        double d = assoc ? ((double) sq * (double) ds) * K_1e4
                         : (double) sq * ((double) ds * K_1e4);
        float decel = (float) d;
        if (v > 0) { v -= decel; if (v < 0.001f) v = 0; }
        else       { v += decel; if (v > -0.001f) v = 0; }
    }
    return v;
}

static int same(float a, float b) {
    if (a != a && b != b) return 1;          /* both NaN (payloads not compared) */
    return ubits(a) == ubits(b);
}

#define NT 16
static const float DS[] = { 0.0f, 1.0f, 10.0f, 30.0f };
#define NDS (sizeof DS / sizeof DS[0])

struct job { int t; uint64_t drag_bad[NDS][2]; uint64_t uw_bad, rad_bad, drag_nonzero[NDS];
             uint32_t ex_drag, ex_uw, ex_rad; float ex_ds; };

static void *work(void *p) {
    struct job *j = p;
    uint64_t lo = ((uint64_t) 1 << 32) * j->t / NT, hi = ((uint64_t) 1 << 32) * (j->t + 1) / NT;
    for (uint64_t i = lo; i < hi; i++) {
        float x = fbits((uint32_t) i);
        for (unsigned k = 0; k < NDS; k++) {
            float o = orig_drag(x, DS[k]);
            if (o != x) j->drag_nonzero[k]++;
            for (int a = 0; a < 2; a++)
                if (!same(o, new_drag(x, DS[k], a))) {
                    if (!j->drag_bad[k][a]++ ) { j->ex_drag = (uint32_t) i; j->ex_ds = DS[k]; }
                }
        }
        float uo = (float) ((q) x * Q_0_01), un = (float) ((double) x * K_0_01);
        if (!same(uo, un) && !j->uw_bad++) j->ex_uw = (uint32_t) i;
        int ro = (q) x > Q_0_1, rn = x >= 0.1f;
        if (ro != rn && !j->rad_bad++) j->ex_rad = (uint32_t) i;
    }
    return NULL;
}

int main(void) {
    Q_1e4  = qbits(0x3ff1a36e2eb1c432ULL, 0xca57a786c226809dULL);
    Q_1e3  = qbits(0x3ff50624dd2f1a9fULL, 0xbe76c8b439581062ULL);
    Q_0_01 = qbits(0x3ff847ae147ae147ULL, 0xae147ae147ae147bULL);
    Q_0_1  = qbits(0x3ffb999999999999ULL, 0x999999999999999aULL);
    /* The binary's constants are the correctly rounded quad literals. */
    if (Q_1e4 != 1e-4Q || Q_1e3 != 1e-3Q || Q_0_01 != 1e-2Q || Q_0_1 != 1e-1Q) {
        puts("FAIL: constant bit patterns do not match the quad literals"); return 2;
    }
    pthread_t th[NT]; struct job jb[NT];
    memset(jb, 0, sizeof jb);
    for (int t = 0; t < NT; t++) { jb[t].t = t; pthread_create(&th[t], NULL, work, &jb[t]); }
    uint64_t dbad[NDS][2] = {{0}}, dnz[NDS] = {0}, uw = 0, rad = 0;
    for (int t = 0; t < NT; t++) {
        pthread_join(th[t], NULL);
        for (unsigned k = 0; k < NDS; k++) {
            dbad[k][0] += jb[t].drag_bad[k][0]; dbad[k][1] += jb[t].drag_bad[k][1];
            dnz[k] += jb[t].drag_nonzero[k];
            if (jb[t].drag_bad[k][0] || jb[t].drag_bad[k][1])
                printf("  example drag mismatch: v=0x%08x ds=%g\n", jb[t].ex_drag, jb[t].ex_ds);
        }
        uw += jb[t].uw_bad; rad += jb[t].rad_bad;
        if (jb[t].uw_bad)  printf("  example underwater mismatch: A=0x%08x\n", jb[t].ex_uw);
        if (jb[t].rad_bad) printf("  example radius mismatch: r=0x%08x\n", jb[t].ex_rad);
    }
    int fail = 0;
    for (unsigned k = 0; k < NDS; k++) {
        printf("drag ds=%-4g: 2^32 inputs, value changed on %llu; mismatches assoc(sq*(ds*k))=%llu assoc((sq*ds)*k)=%llu\n",
               DS[k], (unsigned long long) dnz[k], (unsigned long long) dbad[k][0], (unsigned long long) dbad[k][1]);
        fail |= dbad[k][0] || dbad[k][1];
        if (DS[k] != 0 && dnz[k] < 1000000) { puts("FAIL: vacuous drag coverage"); fail = 1; }
    }
    printf("underwater decelY: 2^32 inputs, mismatches=%llu\n", (unsigned long long) uw);
    printf("wall radius compare: 2^32 inputs, mismatches=%llu\n", (unsigned long long) rad);
    fail |= uw || rad;
    puts(fail ? "RESULT: FAIL" : "RESULT: PASS (bit-exact on every input)");
    return fail;
}
