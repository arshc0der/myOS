/*
 * fbcon.c - console back end that draws text on the framebuffer.
 *
 * This is what makes MyOS show text on UEFI machines (UTM on a Mac, modern
 * PCs) where the old VGA text mode does not exist.
 */
#include "fbcon.h"
#include "tgrid.h"
#include "gfx.h"
#include "sched.h"

#define CELL_W 8
#define CELL_H 16
#define SCROLLBACK 400

static struct tgrid tg;
static bool ready;

bool fbcon_available(void) { return gfx_available(); }
int fbcon_cols(void) { return tg.cols; }
int fbcon_rows(void) { return tg.rows; }

/* Before a live scroll: draw pending rows, then shift the pixels up. */
static void before_scroll(struct tgrid *g)
{
    struct gfx_surface *s = gfx_screen();
    int f, l;

    tg_render(g, s, 0, 0, CELL_H, true, &f, &l);
    gfx_blit(s, 0, 0, s, 0, CELL_H, g->cols * CELL_W, (g->rows - 1) * CELL_H);
}

static bool setup(void)
{
    if (ready)
        return true;
    if (!gfx_available())
        return false;
    int cols = gfx_width() / CELL_W, rows = gfx_height() / CELL_H;

    if (!tg_init(&tg, cols, rows, SCROLLBACK))
        return false;
    tg.scroll_hook = before_scroll;
    ready = true;
    return true;
}

static void fb_clear(void)
{
    if (!setup())
        return;
    tg_clear(&tg);
    gfx_fill_rect(gfx_screen(), 0, 0, gfx_width(), gfx_height(), GFX_BLACK);
    gfx_flush_all();
}

static void fb_putc(char c)
{
    if (!ready)
        return;
    preempt_disable();
    tg_putc(&tg, c);
    preempt_enable();
}

static void fb_set_color(uint8_t a)
{
    if (ready)
        tg.attr = a;
}

static void fb_scroll(int lines)
{
    if (!ready)
        return;
    preempt_disable();
    tg_scroll_view(&tg, lines);
    preempt_enable();
}

static void fb_flush(void)
{
    int f, l;

    if (!ready)
        return;
    preempt_disable();
    tg_render(&tg, gfx_screen(), 0, 0, CELL_H, true, &f, &l);
    if (l >= f)
        gfx_flush(0, f * CELL_H, tg.cols * CELL_W, (l - f + 1) * CELL_H);
    preempt_enable();
}

const struct console_backend fbcon_console = {
    .name = "framebuffer",
    .putc = fb_putc,
    .clear = fb_clear,
    .set_color = fb_set_color,
    .scroll = fb_scroll,
    .flush = fb_flush,
};
