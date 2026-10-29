/* tgrid.c - text grid with scrollback. */
#include "tgrid.h"
#include "heap.h"
#include "kstring.h"

static const uint32_t vga_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

uint32_t tg_color(uint8_t i) { return vga_rgb[i & 15]; }

static struct tcell *line_ptr(struct tgrid *tg, int ring_line)
{
    return &tg->cells[(ring_line % tg->lines) * tg->cols];
}

static struct tcell *live_line(struct tgrid *tg, int row)
{
    return line_ptr(tg, tg->head + row);
}

static void clear_line(struct tgrid *tg, struct tcell *l)
{
    for (int x = 0; x < tg->cols; x++) {
        l[x].ch = ' ';
        l[x].attr = tg->attr;
    }
}

bool tg_init(struct tgrid *tg, int cols, int rows, int scrollback)
{
    memset(tg, 0, sizeof *tg);
    tg->cols = cols;
    tg->rows = rows;
    tg->lines = rows + scrollback;
    tg->cells = kmalloc((size_t)tg->lines * (size_t)cols * sizeof(struct tcell));
    tg->dirty = kzalloc((size_t)rows);
    if (!tg->cells || !tg->dirty) {
        kfree(tg->cells);
        kfree(tg->dirty);
        return false;
    }
    tg->attr = 0x07;
    tg_clear(tg);
    return true;
}

void tg_free(struct tgrid *tg)
{
    kfree(tg->cells);
    kfree(tg->dirty);
    tg->cells = NULL;
    tg->dirty = NULL;
}

void tg_mark_all_dirty(struct tgrid *tg)
{
    memset(tg->dirty, 1, (size_t)tg->rows);
}

void tg_clear(struct tgrid *tg)
{
    for (int i = 0; i < tg->lines; i++)
        clear_line(tg, line_ptr(tg, i));
    tg->head = 0;
    tg->above = 0;
    tg->view = 0;
    tg->row = tg->col = 0;
    tg->last_cursor_row = 0;
    tg_mark_all_dirty(tg);
}

static void scroll_live(struct tgrid *tg)
{
    if (tg->scroll_hook && tg->view == 0)
        tg->scroll_hook(tg);
    tg->head = (tg->head + 1) % tg->lines;
    if (tg->above < tg->lines - tg->rows)
        tg->above++;
    clear_line(tg, live_line(tg, tg->rows - 1));
    tg->row = tg->rows - 1;
    if (tg->view != 0 || !tg->scroll_hook)
        tg_mark_all_dirty(tg);          /* the consumer cannot shift pixels */
    else
        tg->dirty[tg->rows - 1] = 1;
}

void tg_putc(struct tgrid *tg, char c)
{
    if (tg->view != 0) {                /* typing jumps back to the live screen */
        tg->view = 0;
        tg_mark_all_dirty(tg);
    }
    tg->dirty[tg->last_cursor_row] = 1;

    if (c == '\n') {
        tg->col = 0;
        tg->row++;
    } else if (c == '\r') {
        tg->col = 0;
    } else if (c == '\b') {
        if (tg->col > 0) {
            tg->col--;
        } else if (tg->row > 0) {
            tg->row--;
            tg->col = tg->cols - 1;
        }
    } else if (c == '\t') {
        int n = 4 - (tg->col % 4);
        while (n--)
            tg_putc(tg, ' ');
        return;
    } else {
        struct tcell *cell = &live_line(tg, tg->row)[tg->col];
        cell->ch = (uint8_t)c;
        cell->attr = tg->attr;
        tg->dirty[tg->row] = 1;
        if (++tg->col >= tg->cols) {
            tg->col = 0;
            tg->row++;
        }
    }
    if (tg->row >= tg->rows)
        scroll_live(tg);
    tg->dirty[tg->row] = 1;
    tg->last_cursor_row = tg->row;
}

void tg_scroll_view(struct tgrid *tg, int lines)
{
    int old = tg->view;

    if (lines > 0)
        tg->view = MIN(tg->view + lines, tg->above);
    else
        tg->view = MAX(tg->view + lines, 0);
    if (tg->view != old)
        tg_mark_all_dirty(tg);
}

const struct tcell *tg_cell(const struct tgrid *tg, int row, int col)
{
    int ring = tg->head + tg->lines - tg->view + row;

    return &tg->cells[(ring % tg->lines) * tg->cols + col];
}

void tg_render(struct tgrid *tg, struct gfx_surface *s, int ox, int oy,
               int cell_h, bool only_dirty, int *first, int *last)
{
    int f = tg->rows, l = -1;

    for (int y = 0; y < tg->rows; y++) {
        if (only_dirty && !tg->dirty[y])
            continue;
        tg->dirty[y] = 0;
        if (y < f) f = y;
        if (y > l) l = y;
        for (int x = 0; x < tg->cols; x++) {
            const struct tcell *c = tg_cell(tg, y, x);
            uint32_t fg = tg_color(c->attr & 15), bg = tg_color(c->attr >> 4);
            int px = ox + x * 8, py = oy + y * cell_h;
            if (cell_h == 16)
                gfx_char_tall(s, px, py, (char)c->ch, fg, bg);
            else
                gfx_char(s, px, py, (char)c->ch, fg, bg, 1);
        }
    }
    if (tg->view == 0 && tg->row < tg->rows) {          /* cursor: underline block */
        int px = ox + tg->col * 8, py = oy + tg->row * cell_h;
        gfx_fill_rect(s, px, py + cell_h - 3, 8, 2, GFX_RGB(220, 220, 220));
        if (tg->row < f) f = tg->row;
        if (tg->row > l) l = tg->row;
    }
    *first = f;
    *last = l;
}
