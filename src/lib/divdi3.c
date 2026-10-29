/*
 * divdi3.c - 64-bit division helpers.
 *
 * A 32-bit CPU cannot divide 64-bit numbers in one instruction, so the
 * compiler calls these functions when C code does `uint64_t / uint64_t`.
 * Normally libgcc supplies them; we provide our own so the kernel links
 * even when no 32-bit libgcc is installed.
 */
#include "types.h"

uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;

    if (d == 0) {                       /* division by zero: return all ones */
        if (rem)
            *rem = n;
        return ~0ull;
    }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1ull << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d) { return __udivmoddi4(n, d, 0); }

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r;
    __udivmoddi4(n, d, &r);
    return r;
}

int64_t __divdi3(int64_t n, int64_t d)
{
    bool neg = (n < 0) != (d < 0);
    uint64_t un = n < 0 ? -(uint64_t)n : (uint64_t)n;
    uint64_t ud = d < 0 ? -(uint64_t)d : (uint64_t)d;
    uint64_t q = __udivmoddi4(un, ud, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t n, int64_t d)
{
    uint64_t un = n < 0 ? -(uint64_t)n : (uint64_t)n;
    uint64_t ud = d < 0 ? -(uint64_t)d : (uint64_t)d;
    uint64_t r;
    __udivmoddi4(un, ud, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}
