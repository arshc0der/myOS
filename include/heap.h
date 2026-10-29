/*
 * heap.h - kmalloc / kfree, the kernel's malloc.
 *
 *     struct thing *t = kmalloc(sizeof *t);
 *     if (!t) return -ENOMEM;
 *     ...
 *     kfree(t);
 */
#ifndef HEAP_H
#define HEAP_H
#include "types.h"

void  heap_init(void);
void *kmalloc(size_t size);             /* NULL on failure */
void *kzalloc(size_t size);             /* zero-filled     */
void *kcalloc(size_t n, size_t size);
void *krealloc(void *p, size_t size);
void  kfree(void *p);

struct heap_stats {
    uint32_t total_bytes;       /* size of all heap regions   */
    uint32_t used_bytes;        /* currently allocated        */
    uint32_t free_bytes;
    uint32_t alloc_count;       /* live allocations           */
    uint32_t largest_free;
};
void heap_get_stats(struct heap_stats *s);
/* Walk every block and verify the headers. Returns number of problems. */
int  heap_check(void);

#endif
