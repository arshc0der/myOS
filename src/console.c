
#include <stddef.h>
#include <stdint.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static volatile uint16_t *const VGA =
    (volatile uint16_t *)0xB8000;

static size_t row = 0;
static size_t col = 0;
static uint8_t color = 0x07;

static uint16_t entry(char c)
{
    return (uint16_t)(uint8_t)c |
           ((uint16_t)color << 8);
}

static void scroll(void)
{
    for (size_t y = 1; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            VGA[(y - 1) * VGA_WIDTH + x] =
                VGA[y * VGA_WIDTH + x];
        }
    }

    for (size_t x = 0; x < VGA_WIDTH; x++) {
        VGA[(VGA_HEIGHT - 1) * VGA_WIDTH + x] =
            entry(' ');
    }

    row = VGA_HEIGHT - 1;
}

void console_clear(void)
{
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA[i] = entry(' ');

    row = 0;
    col = 0;
}

void console_putc(char c)
{
    if (c == '\n') {
        col = 0;
        row++;
    } else if (c == '\r') {
        col = 0;
    } else if (c == '\b') {
        if (col > 0) {
            col--;
            VGA[row * VGA_WIDTH + col] = entry(' ');
        }
    } else if (c == '\t') {
        size_t spaces = 4 - (col % 4);

        while (spaces--) {
            console_putc(' ');
        }
        return;
    } else {
        VGA[row * VGA_WIDTH + col] = entry(c);
        col++;

        if (col >= VGA_WIDTH) {
            col = 0;
            row++;
        }
    }

    if (row >= VGA_HEIGHT)
        scroll();
}

void console_write(const char *s)
{
    if (!s)
        return;

    while (*s)
        console_putc(*s++);
}
