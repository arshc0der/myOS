/*
 * vmm.c - paging with an identity map.
 *
 * The page directory has 1024 entries, each covering 4 MiB. We start with
 * every entry mapping its 4 MiB straight to the same physical address
 * (a "large page"). The first 4 MiB is split into 4 KiB pages so that
 * page 0 can be left unmapped. vmm_map() splits other large pages on demand.
 */
#include "vmm.h"
#include "pmm.h"
#include "isr.h"
#include "io.h"
#include "kstring.h"
#include "panic.h"
#include "log.h"
#include "kprintf.h"

#define P_PRESENT 0x001
#define P_WRITE   0x002
#define P_USER    0x004
#define P_PCD     0x010
#define P_LARGE   0x080

static uint32_t *pd;                    /* page directory (page aligned) */
static bool enabled;

static uint32_t *new_table(void)
{
    paddr_t p = pmm_alloc_page();
    if (!p)
        return NULL;
    memset((void *)p, 0, PAGE_SIZE);
    return (uint32_t *)p;
}

/* Turn a 4 MiB large page into 1024 small pages with the same mapping. */
static int split_large(uint32_t idx)
{
    uint32_t e = pd[idx];
    uint32_t *t = new_table();

    if (!t)
        return -1;
    uint32_t base = e & 0xFFC00000u;
    uint32_t flags = e & 0x1F;          /* P, W, U, PWT, PCD */
    for (uint32_t i = 0; i < 1024; i++)
        t[i] = (base + i * PAGE_SIZE) | flags;
    pd[idx] = (uint32_t)t | (e & 0x1F) | P_PRESENT;
    return 0;
}

static void page_fault(struct regs *r)
{
    uint32_t addr = read_cr2();
    char msg[96];

    ksnprintf(msg, sizeof msg, "Page fault at 0x%08x (%s, %s%s)", addr,
              (r->err_code & 1) ? "protection violation" : "page not present",
              (r->err_code & 2) ? "write" : "read",
              (r->err_code & 4) ? ", user mode" : "");
    kernel_panic_regs(msg, r);
}

void vmm_init(void)
{
    pd = new_table();
    if (!pd)
        PANIC("vmm: cannot allocate page directory");

    for (uint32_t i = 0; i < 1024; i++)
        pd[i] = (i << 22) | P_PRESENT | P_WRITE | P_LARGE;

    /* First 4 MiB: 4 KiB pages, with page 0 left unmapped. */
    if (split_large(0) != 0)
        PANIC("vmm: out of memory");
    uint32_t *t0 = (uint32_t *)(pd[0] & 0xFFFFF000u);
    t0[0] = 0;

    isr_register(14, page_fault);

    write_cr4(read_cr4() | 0x10);                   /* PSE: large pages */
    write_cr3((uint32_t)pd);
    write_cr0(read_cr0() | 0x80000000u | 0x10000);  /* PG + WP          */
    enabled = true;
}

bool vmm_enabled(void) { return enabled; }

static uint32_t *get_pte(vaddr_t v, bool create)
{
    uint32_t idx = v >> 22;

    if (!(pd[idx] & P_PRESENT)) {
        if (!create)
            return NULL;
        uint32_t *t = new_table();
        if (!t)
            return NULL;
        pd[idx] = (uint32_t)t | P_PRESENT | P_WRITE | P_USER;
    } else if (pd[idx] & P_LARGE) {
        if (split_large(idx) != 0)
            return NULL;
    }
    uint32_t *t = (uint32_t *)(pd[idx] & 0xFFFFF000u);
    return &t[(v >> 12) & 0x3FF];
}

int vmm_map(vaddr_t virt, paddr_t phys, uint32_t flags)
{
    uint32_t *pte = get_pte(virt, true);

    if (!pte)
        return -1;
    *pte = (phys & 0xFFFFF000u) | P_PRESENT | (flags & 0x1F);
    invlpg(virt);
    return 0;
}

int vmm_map_range(vaddr_t virt, paddr_t phys, uint32_t size, uint32_t flags)
{
    uint32_t off = 0;
    for (; off < size; off += PAGE_SIZE)
        if (vmm_map(PAGE_ALIGN_DOWN(virt) + off,
                    PAGE_ALIGN_DOWN(phys) + off, flags) != 0)
            return -1;
    return 0;
}

void vmm_unmap(vaddr_t virt)
{
    uint32_t *pte = get_pte(virt, true);

    if (pte) {
        *pte = 0;
        invlpg(virt);
    }
}

bool vmm_is_mapped(vaddr_t virt)
{
    uint32_t e = pd[virt >> 22];

    if (!(e & P_PRESENT))
        return false;
    if (e & P_LARGE)
        return true;
    return (((uint32_t *)(e & 0xFFFFF000u))[(virt >> 12) & 0x3FF] & P_PRESENT) != 0;
}

paddr_t vmm_get_phys(vaddr_t virt)
{
    uint32_t e = pd[virt >> 22];

    if (!(e & P_PRESENT))
        return 0;
    if (e & P_LARGE)
        return (e & 0xFFC00000u) | (virt & 0x3FFFFF);
    uint32_t pte = ((uint32_t *)(e & 0xFFFFF000u))[(virt >> 12) & 0x3FF];
    if (!(pte & P_PRESENT))
        return 0;
    return (pte & 0xFFFFF000u) | (virt & 0xFFF);
}
