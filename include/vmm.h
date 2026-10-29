/*
 * vmm.h - virtual memory (paging).
 *
 * Paging lets the CPU translate addresses. MyOS keeps an "identity map":
 * virtual address == physical address, so kernel code stays simple. Paging
 * still gives you protection (page 0 is unmapped, so NULL pointer bugs
 * crash cleanly) and the vmm_map() calls you need to build per-process
 * address spaces or map hardware such as the framebuffer.
 */
#ifndef VMM_H
#define VMM_H
#include "types.h"

#define VMM_WRITE  0x002
#define VMM_USER   0x004
#define VMM_NOCACHE 0x010

void    vmm_init(void);
int     vmm_map(vaddr_t virt, paddr_t phys, uint32_t flags);
int     vmm_map_range(vaddr_t virt, paddr_t phys, uint32_t size, uint32_t flags);
void    vmm_unmap(vaddr_t virt);
bool    vmm_is_mapped(vaddr_t virt);
paddr_t vmm_get_phys(vaddr_t virt);         /* 0 if not mapped */
bool    vmm_enabled(void);

#endif
