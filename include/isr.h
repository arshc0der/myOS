/*
 * isr.h - interrupts and CPU exceptions.
 *
 * An "interrupt" makes the CPU pause what it is doing and run a handler.
 * Hardware devices raise IRQs (timer, keyboard, disk); the CPU raises
 * exceptions (divide by zero, page fault); software uses `int 0x80`
 * for system calls.
 *
 * To react to one, write a function and register it:
 *
 *     static void my_handler(struct regs *r) { ... }
 *     irq_register(1, my_handler);        // IRQ 1 = keyboard
 */
#ifndef ISR_H
#define ISR_H
#include "types.h"

/* CPU state saved when an interrupt happens. */
struct regs {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
    uint32_t useresp, ss;       /* only valid when coming from ring 3 */
};

typedef void (*isr_handler_t)(struct regs *r);

#define IRQ_BASE      32            /* IRQ n arrives as vector 32+n */
#define VEC_SYSCALL   0x80

void idt_init(void);
void isr_register(uint8_t vector, isr_handler_t h);
void irq_register(uint8_t irq, isr_handler_t h);    /* irq 0..15 */
void irq_unregister(uint8_t irq);

#endif
