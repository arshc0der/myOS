/*
 * pmm.h - physical memory manager.
 *
 * RAM is divided into 4 KiB "pages". The PMM remembers which pages are
 * free (one bit per page) and hands them out. Everything else that needs
 * memory (the heap, thread stacks, ...) ultimately gets it from here.
 */
#ifndef PMM_H
#define PMM_H
#include "types.h"

void     pmm_init(void);
paddr_t  pmm_alloc_page(void);                  /* 0 on failure */
paddr_t  pmm_alloc_contiguous(uint32_t pages);  /* 0 on failure */
void     pmm_free_page(paddr_t p);
void     pmm_free_pages(paddr_t p, uint32_t pages);
void     pmm_reserve_range(paddr_t start, paddr_t end);

uint32_t pmm_total_pages(void);     /* RAM the kernel can see           */
uint32_t pmm_free_pages_count(void);
uint32_t pmm_used_pages(void);

#endif
