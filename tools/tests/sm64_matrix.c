/* Exact fixed-point matrix checks, including destination/source aliasing. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "pocket/fx32_mtx.h"
f32 gSineTable[0x1400];
static uint32_t seed = 0x12674839;
static int32_t next(void) { seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5; return seed; }
static void reference(Mtx4 out, Mtx4 a, Mtx4 b) {
    for (int r=0;r<4;r++) {
        for (int c=0;c<3;c++) {
            uint64_t sum=0;
            for (int k=0;k<3;k++) sum+=(uint64_t)((int64_t)a[r][k]*b[k][c]);
            out[r][c]=(uint32_t)(sum>>16)+(r==3?(uint32_t)b[3][c]:0u);
        }
        out[r][3]=r==3?65536:0;
    }
}
static void check(void) {
    for (unsigned i=0;i<100000;i++) {
        Mtx4 a,b,out,ref,alias;
        for(int r=0;r<4;r++) for(int c=0;c<4;c++) { a[r][c]=next(); b[r][c]=next(); }
        reference(ref,a,b);
        mtx4_mul(out,a,b); assert(!memcmp(out,ref,sizeof(out)));
        memcpy(alias,a,sizeof(alias)); mtx4_mul(alias,alias,b); assert(!memcmp(alias,ref,sizeof(alias)));
        memcpy(alias,b,sizeof(alias)); mtx4_mul(alias,a,alias); assert(!memcmp(alias,ref,sizeof(alias)));
        reference(ref,a,a); memcpy(alias,a,sizeof(alias)); mtx4_mul(alias,alias,alias); assert(!memcmp(alias,ref,sizeof(alias)));
        Vec3fx pos={next(),next(),next()};
        gSineTable[0]=0; gSineTable[0x400]=1;
        mtx4_billboard(out,a,pos,0);
        memcpy(alias,a,sizeof(alias)); mtx4_billboard(alias,alias,pos,0);
        assert(!memcmp(out,alias,sizeof(out)));
    }
    puts("matrix: 100000 random cases, all alias modes and billboards passed");
}
static void bench(void) {
    Mtx4 a,b,out;
    for(int r=0;r<4;r++) for(int c=0;c<4;c++) { a[r][c]=next(); b[r][c]=next(); }
    struct timespec t0,t1;
    clock_gettime(CLOCK_MONOTONIC,&t0);
    for(unsigned i=0;i<3000000;i++) { a[3][0]=(int32_t)i; mtx4_mul(out,a,b); }
    clock_gettime(CLOCK_MONOTONIC,&t1);
    printf("matrix_ns %.3f checksum %d\n",((t1.tv_sec-t0.tv_sec)*1e9+t1.tv_nsec-t0.tv_nsec)/3000000.0,out[3][0]);
}
int main(int argc,char **argv) { if(argc>1 && !strcmp(argv[1],"bench"))bench();else check(); }
