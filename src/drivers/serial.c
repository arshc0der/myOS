/* serial.c - COM1 (0x3F8) serial port. Used for logs and testing. */
#include "serial.h"
#include "io.h"

#define COM1 0x3F8
static bool serial_ok;

void serial_init(void)
{
    outb(COM1 + 1, 0x00);       /* no interrupts            */
    outb(COM1 + 3, 0x80);       /* enable divisor latch     */
    outb(COM1 + 0, 0x01);       /* 115200 baud              */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);       /* 8 bits, no parity, 1 stop */
    outb(COM1 + 2, 0xC7);       /* enable FIFO              */
    outb(COM1 + 4, 0x03);       /* RTS/DSR                  */
    serial_ok = true;
}

void serial_putc(char c)
{
    if (!serial_ok)
        return;
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++)
        ;
    if (c == '\n')
        outb(COM1, '\r');
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

bool serial_has_data(void)
{
    return serial_ok && (inb(COM1 + 5) & 1);
}

int serial_getc(void)
{
    return serial_has_data() ? inb(COM1) : -1;
}
