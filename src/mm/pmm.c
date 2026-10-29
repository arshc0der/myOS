/*
 * pmm.c - physical page allocator using a bitmap (1 bit per 4 KiB page,
 * 1 = in use). Covers up to 4 GiB of address space.
 */
#include "pmm.h"
#include "bitmap.h"
#include "multiboot.h"
#include "kstring.h"
#include "io.h"
#include "log.h"

#define MAX_PAGES (1u << 20)                    /* 4 GiB / 4 KiB */

extern char _kernel_start[], _kernel_end[];

static uint32_t map[BITMAP_WORDS(MAX_PAGES)];
static uint32_t total_pages;                    /* usable pages found */
static uint32_t free_count;
static uint32_t search_hint;

static void mark_free(uint32_t page)
{
    if (page < MAX_PAGES && bitmap_test(map, page)) {
        bitmap_clear(map, page);
        free_count++;
    }
}

static void mark_used(uint32_t page)
{
    if (page < MAX_PAGES && !bitmap_test(map, page)) {
        bitmap_set(map, page);
        free_count--;
    }
}

void pmm_reserve_range(paddr_t start, paddr_t end)
{
    for (uint32_t p = start >> PAGE_SHIFT;
         p < (PAGE_ALIGN_UP(end) >> PAGE_SHIFT); p++)
        mark_used(p);
}

void pmm_init(void)
{
    memset(map, 0xFF, sizeof map);              /* everything "used"  */
    free_count = 0;

    if (boot_info.mmap_count > 0) {
        for (int i = 0; i < boot_info.mmap_count; i++) {
            const struct boot_mmap_entry *e = &boot_info.mmap[i];
            if (e->type != MB_MEM_AVAILABLE || e->base >= 0x100000000ull)
                continue;
            uint64_t end = e->base + e->length;
            if (end > 0x100000000ull)
                end = 0x100000000ull;
            uint32_t first = (uint32_t)((e->base + PAGE_SIZE - 1) >> PAGE_SHIFT);
            uint32_t last  = (uint32_t)(end >> PAGE_SHIFT);
            for (uint32_t p = first; p < last; p++) {
                bitmap_clear(map, p);
                free_count++;
            }
        }
    } else {
        /* No memory map: use mem_upper (KiB above 1 MiB). */
        uint32_t first = 0x100000 >> PAGE_SHIFT;
        uint32_t last = (0x100000 + boot_info.mem_upper_kb * 1024) >> PAGE_SHIFT;
        for (uint32_t p = first; p < last; p++) {
            bitmap_clear(map, p);
            free_count++;
        }
    }
    total_pages = free_count;

    /* Never hand out: low 1 MiB, the kernel image, boot structures. */
    pmm_reserve_range(0, 0x100000);
    pmm_reserve_range((paddr_t)_kernel_start, (paddr_t)_kernel_end);
    for (int i = 0; i < boot_info.reserved_count; i++)
        pmm_reserve_range(boot_info.reserved[i].start,
                          boot_info.reserved[i].end);

    search_hint = 0x100000 >> PAGE_SHIFT;
}

paddr_t pmm_alloc_page(void)
{
    uint32_t f = irq_save();
    int32_t p = bitmap_find_clear(map, MAX_PAGES, search_hint);

    if (p < 0)
        p = bitmap_find_clear(map, MAX_PAGES, 0x100000 >> PAGE_SHIFT);
    if (p < 0) {
        irq_restore(f);
        return 0;
    }
    mark_used((uint32_t)p);
    search_hint = (uint32_t)p + 1;
    irq_restore(f);
    return (paddr_t)p << PAGE_SHIFT;
}

paddr_t pmm_alloc_contiguous(uint32_t pages)
{
    uint32_t f = irq_save();
    int32_t p = bitmap_find_clear_run(map, MAX_PAGES, pages);

    /* Skip runs that start in the low 1 MiB (those are always "used"). */
    if (p < 0) {
        irq_restore(f);
        return 0;
    }
    for (uint32_t i = 0; i < pages; i++)
        mark_used((uint32_t)p + i);
    irq_restore(f);
    return (paddr_t)p << PAGE_SHIFT;
}

void pmm_free_page(paddr_t p)
{
    uint32_t f = irq_save();
    mark_free(p >> PAGE_SHIFT);
    if ((p >> PAGE_SHIFT) < search_hint)
        search_hint = p >> PAGE_SHIFT;
    irq_restore(f);
}

void pmm_free_pages(paddr_t p, uint32_t pages)
{
    for (uint32_t i = 0; i < pages; i++)
        pmm_free_page(p + i * PAGE_SIZE);
}

uint32_t pmm_total_pages(void)      { return total_pages; }
uint32_t pmm_free_pages_count(void) { return free_count; }
uint32_t pmm_used_pages(void)       { return total_pages - free_count; }
