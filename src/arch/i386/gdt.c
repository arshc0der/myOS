/* gdt.c - flat 4 GiB segments for ring 0 and ring 3, plus one TSS. */
#include "gdt.h"
#include "kstring.h"

struct gdt_entry {
    uint16_t limit_lo, base_lo;
    uint8_t  base_mid, access, gran, base_hi;
} PACKED;

struct gdt_ptr { uint16_t limit; uint32_t base; } PACKED;

struct tss {
    uint32_t prev, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap_base;
} PACKED;

static struct gdt_entry gdt[6];
static struct gdt_ptr gp;
static struct tss tss;

static void set_gate(int i, uint32_t base, uint32_t limit,
                     uint8_t access, uint8_t gran)
{
    gdt[i].base_lo  = base & 0xFFFF;
    gdt[i].base_mid = (base >> 16) & 0xFF;
    gdt[i].base_hi  = (base >> 24) & 0xFF;
    gdt[i].limit_lo = limit & 0xFFFF;
    gdt[i].gran     = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access   = access;
}

extern void gdt_flush(uint32_t ptr);

void gdt_init(void)
{
    set_gate(0, 0, 0, 0, 0);
    set_gate(1, 0, 0xFFFFF, 0x9A, 0xCF);    /* kernel code */
    set_gate(2, 0, 0xFFFFF, 0x92, 0xCF);    /* kernel data */
    set_gate(3, 0, 0xFFFFF, 0xFA, 0xCF);    /* user code   */
    set_gate(4, 0, 0xFFFFF, 0xF2, 0xCF);    /* user data   */

    memset(&tss, 0, sizeof tss);
    tss.ss0 = SEG_KDATA;
    tss.iomap_base = sizeof tss;
    set_gate(5, (uint32_t)&tss, sizeof tss - 1, 0x89, 0x00);

    gp.limit = sizeof gdt - 1;
    gp.base = (uint32_t)gdt;
    gdt_flush((uint32_t)&gp);
}

void tss_set_kernel_stack(uint32_t esp0)
{
    tss.esp0 = esp0;
}
