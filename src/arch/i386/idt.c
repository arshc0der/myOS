/* idt.c - interrupt descriptor table and the central dispatcher. */
#include "isr.h"
#include "gdt.h"
#include "pic.h"
#include "kstring.h"
#include "panic.h"
#include "io.h"

struct idt_entry {
    uint16_t base_lo, sel;
    uint8_t  zero, flags;
    uint16_t base_hi;
} PACKED;

struct idt_ptr { uint16_t limit; uint32_t base; } PACKED;

static struct idt_entry idt[256];
static struct idt_ptr ip;
static isr_handler_t handlers[256];

extern uint32_t isr_table[];
extern uint32_t isr_table_count;

static const char *exc_names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor overrun", "Invalid TSS",
    "Segment not present", "Stack fault", "General protection fault",
    "Page fault", "Reserved", "x87 floating point", "Alignment check",
    "Machine check", "SIMD floating point", "Virtualization",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Security", "Reserved"
};

static void set_gate(int v, uint32_t base, uint8_t flags)
{
    idt[v].base_lo = base & 0xFFFF;
    idt[v].base_hi = (base >> 16) & 0xFFFF;
    idt[v].sel = SEG_KCODE;
    idt[v].zero = 0;
    idt[v].flags = flags;
}

void idt_init(void)
{
    memset(idt, 0, sizeof idt);
    for (uint32_t i = 0; i < 48; i++)
        set_gate((int)i, isr_table[i], 0x8E);           /* ring 0 only   */
    set_gate(VEC_SYSCALL, isr_table[48], 0xEE);         /* callable from ring 3 */

    ip.limit = sizeof idt - 1;
    ip.base = (uint32_t)idt;
    __asm__ volatile ("lidt %0" : : "m"(ip));
}

void isr_register(uint8_t v, isr_handler_t h) { handlers[v] = h; }
void irq_register(uint8_t irq, isr_handler_t h)
{
    handlers[IRQ_BASE + irq] = h;
    pic_unmask(irq);
}
void irq_unregister(uint8_t irq)
{
    pic_mask(irq);
    handlers[IRQ_BASE + irq] = NULL;
}

/* Called from isr_common (isr_stubs.S) for every interrupt. */
void isr_dispatch(struct regs *r)
{
    uint32_t n = r->int_no;

    if (n >= IRQ_BASE && n < IRQ_BASE + 16) {
        uint8_t irq = (uint8_t)(n - IRQ_BASE);
        /* Spurious IRQ 7 / 15: ignore without acknowledging properly. */
        if (irq == 7) {
            outb(0x20, 0x0B);
            if (!(inb(0x20) & 0x80))
                return;
        }
        /* Acknowledge first: the handler may switch to another task. */
        pic_eoi(irq);
    }

    if (handlers[n]) {
        handlers[n](r);
        return;
    }

    if (n < 32)
        kernel_panic_regs(exc_names[n], r);
}
