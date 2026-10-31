/*
 * tgrid.h - a grid of text cells with a cursor and scrollback.
 *
 * Used by the framebuffer console and by the GUI's Terminal window. It
 * only stores text; tg_render() draws it onto a gfx surface.
 */
#ifndef TGRID_H
#define TGRID_H
#include "types.h"
#include "gfx.h"

struct tcell { uint8_t ch, attr; };

struct tgrid {
    int cols, rows, lines;          /* lines = rows + scrollback          */
    struct tcell *cells;            /* lines * cols, used as a ring       */
    int head;                       /* ring line of the top live row      */
    int above;                      /* history lines above the live rows  */
    int view;                       /* scrolled back this many lines      */
    int row, col;                   /* cursor (live screen)               */
    uint8_t attr;                   /* VGA colour attribute for new text  */
    uint8_t *dirty;                 /* per-row "needs redraw"             */
    int last_cursor_row;
    /* Called just BEFORE the live screen scrolls up by one row. */
    void (*scroll_hook)(struct tgrid *tg);
    void *user;
};

bool tg_init(struct tgrid *tg, int cols, int rows, int scrollback);
void tg_free(struct tgrid *tg);
void tg_putc(struct tgrid *tg, char c);
void tg_clear(struct tgrid *tg);
void tg_scroll_view(struct tgrid *tg, int lines);   /* >0 = back in time */
void tg_mark_all_dirty(struct tgrid *tg);
const struct tcell *tg_cell(const struct tgrid *tg, int row, int col);

extern bool tg_light_mode;                         /* black on white rendering */
uint32_t tg_color(uint8_t vga_index);               /* VGA colour -> 0xRRGGBB */

/*
 * Draw rows onto `s` with the grid's top-left at (ox, oy). Cells are
 * 8 pixels wide and `cell_h` (8 or 16) tall. If only_dirty, rows not
 * marked dirty are skipped. Returns the first and last row drawn
 * (first > last when nothing was drawn).
 */
void tg_render(struct tgrid *tg, struct gfx_surface *s, int ox, int oy,
               int cell_h, bool only_dirty, int *first, int *last);

#endif
