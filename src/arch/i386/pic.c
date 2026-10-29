/* pic.c - 8259 PIC setup. */
#include "pic.h"
#include "io.h"

#define PIC1 0x20
#define PIC2 0xA0

void pic_init(void)
{
    outb(PIC1, 0x11); io_wait();            /* start init (ICW1) */
    outb(PIC2, 0x11); io_wait();
    outb(PIC1 + 1, 32); io_wait();          /* vector offsets    */
    outb(PIC2 + 1, 40); io_wait();
    outb(PIC1 + 1, 4);  io_wait();          /* slave on IRQ2     */
    outb(PIC2 + 1, 2);  io_wait();
    outb(PIC1 + 1, 1);  io_wait();          /* 8086 mode         */
    outb(PIC2 + 1, 1);  io_wait();
    outb(PIC1 + 1, 0xFB);                   /* mask all but cascade */
    outb(PIC2 + 1, 0xFF);
}

void pic_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2, 0x20);
    outb(PIC1, 0x20);
}

void pic_mask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1 + 1 : PIC2 + 1;
    outb(port, inb(port) | (1u << (irq & 7)));
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1 + 1 : PIC2 + 1;
    outb(port, inb(port) & ~(1u << (irq & 7)));
}
