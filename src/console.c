/*
 * console.c - VGA text console with scrollback and a hardware cursor.
 *
 * The screen is 80x25 characters. Video memory at 0xB8000 holds one 16-bit
 * value per character: the low byte is the character, the high byte is the
 * color.
 *
 * Scrollback:
 *   Everything printed is also stored in a ring buffer (sb[]) of SB_LINES
 *   lines. The last 25 lines of the buffer are the "live" screen. Scrolling
 *   back just redraws the screen from older lines in the buffer.
 *
 * Cursor:
 *   The blinking cursor is a feature of the VGA hardware. It does not move
 *   by itself, so we program its position through ports 0x3D4/0x3D5.
 */

#include <stddef.h>
#include <stdint.h>

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define SB_LINES    300             /* screen + history, in lines */

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static volatile uint16_t *const VGA = (volatile uint16_t *)0xB8000;

static uint16_t sb[SB_LINES * VGA_WIDTH];   /* scrollback ring buffer      */
static size_t head;     /* ring index of the first line of the live screen */
static size_t above;    /* history lines available above the live screen   */
static size_t view;     /* lines scrolled back from live (0 = live)        */
static size_t row;      /* cursor row on the live screen                   */
static size_t col;      /* cursor column                                   */
static uint8_t color = 0x07;                /* light grey on black         */

static uint16_t entry(char c)
{
    return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}

/* Pointer to line number `line` of the live screen (0 = top). */
static uint16_t *live_line(size_t line)
{
    return &sb[((head + line) % SB_LINES) * VGA_WIDTH];
}

static void clear_line(uint16_t *line)
{
    for (size_t x = 0; x < VGA_WIDTH; x++)
        line[x] = entry(' ');
}

/* ---------- hardware cursor ---------- */

static void update_cursor(void)
{
    if (view != 0) {                    /* hide it while scrolled back */
        outb(0x3D4, 0x0A);
        outb(0x3D5, 0x20);
        return;
    }

    uint16_t pos = (uint16_t)(row * VGA_WIDTH + col);

    outb(0x3D4, 0x0A);  outb(0x3D5, 13);            /* underline cursor   */
    outb(0x3D4, 0x0B);  outb(0x3D5, 15);
    outb(0x3D4, 0x0F);  outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);  outb(0x3D5, (uint8_t)(pos >> 8));
}

/* ---------- drawing ---------- */

/* Small "[ SCROLLBACK -NN ]" label shown while looking at old output. */
static void draw_indicator(void)
{
    char text[24];
    size_t len = 0;
    const char *prefix = "[ SCROLLBACK -";

    while (*prefix)
        text[len++] = *prefix++;

    char digits[4];
    size_t nd = 0;
    size_t n = view;

    do {
        digits[nd++] = (char)('0' + n % 10);
        n /= 10;
    } while (n > 0);

    while (nd > 0)
        text[len++] = digits[--nd];

    text[len++] = ' ';
    text[len++] = ']';

    for (size_t i = 0; i < len; i++)
        VGA[VGA_WIDTH - len + i] =
            (uint16_t)(uint8_t)text[i] | (uint16_t)(0x70 << 8);
}

/* Copy the visible part of the buffer to video memory. */
static void render(void)
{
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        size_t idx = (head + SB_LINES - view + y) % SB_LINES;
        const uint16_t *src = &sb[idx * VGA_WIDTH];

        for (size_t x = 0; x < VGA_WIDTH; x++)
            VGA[y * VGA_WIDTH + x] = src[x];
    }

    if (view != 0)
        draw_indicator();
}

/* Typing or printing always jumps back to the live screen. */
static void snap_to_live(void)
{
    if (view != 0) {
        view = 0;
        render();
    }
}

/* Push the live screen up by one line. */
static void scroll_live(void)
{
    head = (head + 1) % SB_LINES;

    if (above < SB_LINES - VGA_HEIGHT)
        above++;

    clear_line(live_line(VGA_HEIGHT - 1));
    row = VGA_HEIGHT - 1;
    render();
}

/* ---------- output ---------- */

static void putc_raw(char c)
{
    if (c == '\n') {
        col = 0;
        row++;
    } else if (c == '\r') {
        col = 0;
    } else if (c == '\b') {
        /* Move left only; the caller prints a space to erase. */
        if (col > 0) {
            col--;
        } else if (row > 0) {
            row--;
            col = VGA_WIDTH - 1;
        }
    } else if (c == '\t') {
        size_t spaces = 4 - (col % 4);

        while (spaces--)
            putc_raw(' ');
        return;
    } else {
        uint16_t e = entry(c);

        live_line(row)[col] = e;
        VGA[row * VGA_WIDTH + col] = e;
        col++;

        if (col >= VGA_WIDTH) {
            col = 0;
            row++;
        }
    }

    if (row >= VGA_HEIGHT)
        scroll_live();
}

void console_clear(void)
{
    for (size_t i = 0; i < SB_LINES * VGA_WIDTH; i++)
        sb[i] = entry(' ');

    head = 0;
    above = 0;
    view = 0;
    row = 0;
    col = 0;

    render();
    update_cursor();
}

void console_putc(char c)
{
    snap_to_live();
    putc_raw(c);
    update_cursor();
}

void console_write(const char *s)
{
    if (!s)
        return;

    snap_to_live();

    while (*s)
        putc_raw(*s++);

    update_cursor();
}

void console_set_color(uint8_t c)
{
    color = c;
}

/*
 * Scroll the view. Positive = towards older output (up),
 * negative = towards newer output (down).
 */
void console_scroll(int lines)
{
    if (lines > 0) {
        size_t n = (size_t)lines;

        view = (view + n > above) ? above : view + n;
    } else {
        size_t n = (size_t)(-lines);

        view = (n > view) ? 0 : view - n;
    }

    render();
    update_cursor();
}

/* ---------- number printing (there is no printf) ---------- */

void console_put_dec(uint32_t value)
{
    char buf[11];
    size_t n = 0;

    if (value == 0) {
        console_putc('0');
        return;
    }

    while (value > 0) {
        buf[n++] = (char)('0' + value % 10);
        value /= 10;
    }

    while (n > 0)
        console_putc(buf[--n]);
}

/* Prints `digits` hex digits with no prefix, e.g. 00FF. */
void console_put_hex_plain(uint32_t value, int digits)
{
    const char *hex = "0123456789ABCDEF";

    for (int i = digits - 1; i >= 0; i--)
        console_putc(hex[(value >> (i * 4)) & 0xF]);
}

/* Prints 0x followed by `digits` hex digits, e.g. 0x00FF. */
void console_put_hex(uint32_t value, int digits)
{
    console_write("0x");
    console_put_hex_plain(value, digits);
}

/* Prints a 64-bit value as 0x + 16 hex digits, given as two halves. */
void console_put_hex64(uint32_t hi, uint32_t lo)
{
    console_write("0x");
    console_put_hex_plain(hi, 8);
    console_put_hex_plain(lo, 8);
}
