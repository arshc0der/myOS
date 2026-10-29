/*
 * bitmap.h - bit arrays. Handy for tracking free/used things, such as
 * memory pages (pmm.c) or disk blocks in a filesystem.
 */
#ifndef BITMAP_H
#define BITMAP_H

#include "types.h"

/* Number of uint32_t words needed to hold `bits` bits. */
#define BITMAP_WORDS(bits) (((bits) + 31) / 32)

void bitmap_set(uint32_t *map, uint32_t bit);
void bitmap_clear(uint32_t *map, uint32_t bit);
bool bitmap_test(const uint32_t *map, uint32_t bit);

/* Set/clear a range of bits [start, start+count). */
void bitmap_set_range(uint32_t *map, uint32_t start, uint32_t count);
void bitmap_clear_range(uint32_t *map, uint32_t start, uint32_t count);

/* Index of the first clear bit at or after `from`, or -1 if none. */
int32_t bitmap_find_clear(const uint32_t *map, uint32_t bits, uint32_t from);

/* Start of `count` consecutive clear bits, or -1 if there is no such run. */
int32_t bitmap_find_clear_run(const uint32_t *map, uint32_t bits,
                              uint32_t count);

/* Number of set bits in [0, bits). */
uint32_t bitmap_count_set(const uint32_t *map, uint32_t bits);

#endif
