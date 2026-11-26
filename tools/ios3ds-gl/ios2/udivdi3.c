/* 64-bit unsigned division for the 2.x dylib. The prebuilt compiler-rt
 * __udivdi3/__umoddi3 use r9, which 5H11a reserves as the thread pointer
 * (pthread_getspecific, dyld's lazy binder): ca_frame()'s 64-bit divides
 * clobbered it and the next lazy bind on CA's render thread faulted in dyld
 * (0x2fe1e724). Built with -ffixed-r9 like the rest of the dylib.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
static uint64_t udivmod(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    int i;
    if (d == 0) {
        if (rem)
            *rem = 0;
        return ~0ull;
    }
    if (!(n >> 32) && !(d >> 32)) {
        if (rem)
            *rem = (uint32_t)n % (uint32_t)d;
        return (uint32_t)n / (uint32_t)d;
    }
    for (i = 63; i >= 0; i--) {
        r = r << 1 | (n >> i & 1);
        if (r >= d) {
            r -= d;
            q |= 1ull << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}
uint64_t __udivdi3(uint64_t n, uint64_t d) { return udivmod(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; udivmod(n, d, &r); return r; }
