/*
 * vga.c - VGA text-mode console back end (80x25) with scrollback and a
 * hardware cursor. Same design as v1.2.0, packaged as a console back end.
 */
#include "console.h"
#include "io.h"

#define W  80
#define H  25
#define SB_LINES 300

static volatile uint16_t *const VGA = (volatile uint16_t *)0xB8000;
static uint16_t sb[SB_LINES * W];
static size_t head, above, view, row, col;
static uint8_t color = 0x07;

static uint16_t entry(char c) { return (uint16_t)(uint8_t)c | ((uint16_t)color << 8); }
static uint16_t *live_line(size_t l) { return &sb[((head + l) % SB_LINES) * W]; }

static void update_cursor(void)
{
    if (view != 0) {
        outb(0x3D4, 0x0A); outb(0x3D5, 0x20);
        return;
    }
    uint16_t pos = (uint16_t)(row * W + col);
    outb(0x3D4, 0x0A); outb(0x3D5, 13);
    outb(0x3D4, 0x0B); outb(0x3D5, 15);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
}

static void draw_indicator(void)
{
    char t[24];
    size_t len = 0;
    const char *p = "[ SCROLLBACK -";
    while (*p) t[len++] = *p++;
    char d[4]; size_t nd = 0, n = view;
    do { d[nd++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (nd) t[len++] = d[--nd];
    t[len++] = ' '; t[len++] = ']';
    for (size_t i = 0; i < len; i++)
        VGA[W - len + i] = (uint16_t)(uint8_t)t[i] | (0x70 << 8);
}

static void render(void)
{
    for (size_t y = 0; y < H; y++) {
        const uint16_t *src = &sb[((head + SB_LINES - view + y) % SB_LINES) * W];
        for (size_t x = 0; x < W; x++)
            VGA[y * W + x] = src[x];
    }
    if (view)
        draw_indicator();
}

static void snap(void) { if (view) { view = 0; render(); } }

static void scroll_live(void)
{
    head = (head + 1) % SB_LINES;
    if (above < SB_LINES - H)
        above++;
    uint16_t *l = live_line(H - 1);
    for (size_t x = 0; x < W; x++)
        l[x] = entry(' ');
    row = H - 1;
    render();
}

static void vga_putc(char c)
{
    snap();
    if (c == '\n') { col = 0; row++; }
    else if (c == '\r') { col = 0; }
    else if (c == '\b') {
        if (col) col--;
        else if (row) { row--; col = W - 1; }
    } else if (c == '\t') {
        size_t n = 4 - (col % 4);
        while (n--) vga_putc(' ');
        return;
    } else {
        uint16_t e = entry(c);
        live_line(row)[col] = e;
        VGA[row * W + col] = e;
        if (++col >= W) { col = 0; row++; }
    }
    if (row >= H)
        scroll_live();
    update_cursor();
}

static void vga_clear(void)
{
    for (size_t i = 0; i < SB_LINES * W; i++)
        sb[i] = entry(' ');
    head = above = view = row = col = 0;
    render();
    update_cursor();
}

static void vga_set_color(uint8_t c) { color = c; }

static void vga_scroll(int lines)
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

const struct console_backend vga_console = {
    .name = "vga-text",
    .putc = vga_putc,
    .clear = vga_clear,
    .set_color = vga_set_color,
    .scroll = vga_scroll,
};
