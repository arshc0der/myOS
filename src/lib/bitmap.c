/* bitmap.c - bit array helpers. */
#include "bitmap.h"

void bitmap_set(uint32_t *m, uint32_t b)   { m[b / 32] |=  (1u << (b % 32)); }
void bitmap_clear(uint32_t *m, uint32_t b) { m[b / 32] &= ~(1u << (b % 32)); }
bool bitmap_test(const uint32_t *m, uint32_t b)
{
    return (m[b / 32] >> (b % 32)) & 1u;
}

void bitmap_set_range(uint32_t *m, uint32_t start, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        bitmap_set(m, start + i);
}

void bitmap_clear_range(uint32_t *m, uint32_t start, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        bitmap_clear(m, start + i);
}

int32_t bitmap_find_clear(const uint32_t *m, uint32_t bits, uint32_t from)
{
    for (uint32_t i = from; i < bits; ) {
        if (i % 32 == 0 && m[i / 32] == 0xFFFFFFFFu) {  /* skip full word */
            i += 32;
            continue;
        }
        if (!bitmap_test(m, i))
            return (int32_t)i;
        i++;
    }
    return -1;
}

int32_t bitmap_find_clear_run(const uint32_t *m, uint32_t bits, uint32_t count)
{
    uint32_t run = 0, start = 0;

    if (count == 0)
        return -1;
    for (uint32_t i = 0; i < bits; i++) {
        if (bitmap_test(m, i)) {
            run = 0;
        } else {
            if (run == 0)
                start = i;
            if (++run == count)
                return (int32_t)start;
        }
    }
    return -1;
}

uint32_t bitmap_count_set(const uint32_t *m, uint32_t bits)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < bits; i++)
        if (bitmap_test(m, i))
            n++;
    return n;
}
