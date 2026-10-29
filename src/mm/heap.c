/*
 * heap.c - kernel heap: first-fit free list with splitting and merging.
 *
 * Memory is split into blocks. Every block starts with a 16-byte header:
 *
 *     [ header | payload ............ ][ header | payload ... ] ...
 *
 * A free block keeps a list_head inside its payload so free blocks can be
 * linked together. When a block is freed it merges with free neighbours.
 */
#include "heap.h"
#include "pmm.h"
#include "list.h"
#include "kstring.h"
#include "io.h"
#include "log.h"
#include "panic.h"

#define MAGIC     0xB10C0000u
#define MAGIC_END 0xE4D00000u
#define USED_BIT  1u
#define ALIGN     16u

struct block {
    uint32_t magic;
    uint32_t size_flags;        /* payload size | USED_BIT          */
    uint32_t prev_size;         /* payload size of the block before */
    uint32_t pad;
};
#define HDR sizeof(struct block)

struct free_node { struct list_head link; };

static LIST_HEAD(free_list);
static uint32_t total_bytes, used_bytes, alloc_count;
static struct { uint32_t start, end; } regions[8];
static int region_count;

static inline uint32_t bsize(const struct block *b) { return b->size_flags & ~USED_BIT; }
static inline bool bused(const struct block *b) { return b->size_flags & USED_BIT; }
static inline struct block *next_block(struct block *b)
{
    return (struct block *)((uint8_t *)b + HDR + bsize(b));
}
static inline struct block *prev_block(struct block *b)
{
    return (struct block *)((uint8_t *)b - HDR - b->prev_size);
}
static inline struct free_node *fnode(struct block *b)
{
    return (struct free_node *)((uint8_t *)b + HDR);
}
static inline struct block *from_node(struct list_head *l)
{
    return (struct block *)((uint8_t *)l - HDR);
}

/* Add a chunk of memory to the heap as one big free block. */
static void add_region(void *mem, uint32_t bytes)
{
    if (region_count >= 8 || bytes < 4 * HDR + 64)
        return;
    uint8_t *base = mem;
    struct block *first = (struct block *)base;
    uint32_t payload = bytes - 2 * HDR;     /* room for the end marker */

    first->magic = MAGIC;
    first->size_flags = payload;
    first->prev_size = 0;
    list_add(&fnode(first)->link, &free_list);

    struct block *end = next_block(first);  /* zero-size "used" sentinel */
    end->magic = MAGIC_END;
    end->size_flags = USED_BIT;
    end->prev_size = payload;

    regions[region_count].start = (uint32_t)base;
    regions[region_count].end = (uint32_t)base + bytes;
    region_count++;
    total_bytes += payload;
}

void heap_init(void)
{
    uint32_t free_pages = pmm_free_pages_count();
    uint32_t want = free_pages / 4;                 /* a quarter of RAM  */
    if (want > MiB(32) / PAGE_SIZE)
        want = MiB(32) / PAGE_SIZE;
    if (want < 128)
        want = 128;                                 /* at least 512 KiB  */

    paddr_t mem = 0;
    while (want >= 64 && !(mem = pmm_alloc_contiguous(want)))
        want /= 2;
    if (!mem)
        PANIC("heap: cannot allocate heap memory");
    add_region((void *)mem, want * PAGE_SIZE);
}

void *kmalloc(size_t size)
{
    if (size == 0)
        size = 1;
    size = (size + ALIGN - 1) & ~(ALIGN - 1);
    if (size < sizeof(struct free_node))
        size = sizeof(struct free_node);

    uint32_t flags = irq_save();
    struct list_head *p;

    list_for_each(p, &free_list) {
        struct block *b = from_node(p);

        if (bsize(b) < size)
            continue;
        list_del(p);
        /* Split if the remainder can hold a header and a minimal payload. */
        if (bsize(b) >= size + HDR + ALIGN) {
            uint32_t rest = bsize(b) - size - HDR;
            b->size_flags = size | USED_BIT;
            struct block *r = next_block(b);
            r->magic = MAGIC;
            r->size_flags = rest;
            r->prev_size = size;
            next_block(r)->prev_size = rest;
            list_add(&fnode(r)->link, &free_list);
        } else {
            b->size_flags |= USED_BIT;
        }
        used_bytes += bsize(b);
        alloc_count++;
        irq_restore(flags);
        return (uint8_t *)b + HDR;
    }
    irq_restore(flags);
    return NULL;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    struct block *b = (struct block *)((uint8_t *)ptr - HDR);
    uint32_t flags = irq_save();

    if (b->magic != MAGIC || !bused(b))
        PANIC("kfree: bad pointer or double free (%p)", ptr);

    used_bytes -= bsize(b);
    alloc_count--;
    b->size_flags &= ~USED_BIT;

    struct block *n = next_block(b);
    if (!bused(n) && n->magic == MAGIC) {           /* merge with next */
        list_del(&fnode(n)->link);
        b->size_flags = bsize(b) + HDR + bsize(n);
    }
    bool first = false;                             /* region start? */
    for (int i = 0; i < region_count; i++)
        if ((uint32_t)b == regions[i].start)
            first = true;
    if (!first) {                                   /* merge with previous */
        struct block *pb = prev_block(b);
        if (!bused(pb) && pb->magic == MAGIC) {
            list_del(&fnode(pb)->link);
            pb->size_flags = bsize(pb) + HDR + bsize(b);
            b = pb;
        }
    }
    next_block(b)->prev_size = bsize(b);
    list_add(&fnode(b)->link, &free_list);
    irq_restore(flags);
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void *kcalloc(size_t n, size_t size)
{
    if (size && n > 0xFFFFFFFFu / size)
        return NULL;
    return kzalloc(n * size);
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr)
        return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    struct block *b = (struct block *)((uint8_t *)ptr - HDR);
    uint32_t old = bsize(b);

    if (size <= old)
        return ptr;
    void *n = kmalloc(size);
    if (!n)
        return NULL;
    memcpy(n, ptr, old);
    kfree(ptr);
    return n;
}

void heap_get_stats(struct heap_stats *s)
{
    uint32_t flags = irq_save();

    s->total_bytes = total_bytes;
    s->used_bytes = used_bytes;
    s->free_bytes = total_bytes - used_bytes;
    s->alloc_count = alloc_count;
    s->largest_free = 0;

    struct list_head *p;
    list_for_each(p, &free_list)
        s->largest_free = MAX(s->largest_free, bsize(from_node(p)));
    irq_restore(flags);
}

int heap_check(void)
{
    int problems = 0;
    uint32_t flags = irq_save();

    for (int i = 0; i < region_count; i++) {
        struct block *b = (struct block *)regions[i].start;
        uint32_t prev = 0;

        while (b->magic != MAGIC_END) {
            if (b->magic != MAGIC || b->prev_size != prev) {
                problems++;
                break;
            }
            prev = bsize(b);
            b = next_block(b);
            if ((uint32_t)b >= regions[i].end) {
                problems++;
                break;
            }
        }
    }
    irq_restore(flags);
    return problems;
}
