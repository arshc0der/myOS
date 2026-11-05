/*
 * gallery.c - "Gallery": every widget of the GUI API in one window. Read this file to see how
 * layout, check boxes, switches, sliders, progress bars, tabs, pictures and scroll views are used.
 * It is an ordinary app: copy it as the start of your own.
 */
#include "guiapi.h"
#include "input.h"
#include "heap.h"
#include "kprintf.h"
#include "kstring.h"

struct gal {
    int tab;
    bool check, sw;
    int slider;
    struct gui_scroll sv;
    struct gfx_surface *content, *pic;
};

static void gal_open(struct gui_window *w)
{
    struct gal *g = kzalloc(sizeof *g);

    w->priv = g;
    if (!g)
        return;
    g->slider = 40;
    g->content = gfx_surface_create(700, 400);          /* bigger than the view, so both bars appear */
    g->pic = gfx_surface_create(160, 100);
    if (g->pic)
        for (int y = 0; y < 100; y++)
            for (int x = 0; x < 160; x++)
                g->pic->px[y * 160 + x] = GFX_RGB(x * 255 / 160, y * 255 / 100, 180);
    if (g->content) {
        gfx_fill_rect(g->content, 0, 0, 700, 400, GFX_RGB(245, 245, 247));
        for (int i = 0; i < 20; i++) {
            char t[40];
            gfx_fill_rect(g->content, 10, 10 + i * 19, 680, 18, i % 2 ? GFX_RGB(235, 238, 245) : GFX_RGB(250, 250, 252));
            ksnprintf(t, sizeof t, "Row %d of a scrolling list - wide content to the right ->", i + 1);
            gfx_text(g->content, 16, 15 + i * 19, t, GFX_RGB(40, 40, 46), GFX_TRANSPARENT, 1);
            gfx_text(g->content, 520, 15 + i * 19, "far right", GFX_RGB(120, 120, 130), GFX_TRANSPARENT, 1);
        }
    }
}

static void gal_close(struct gui_window *w)
{
    struct gal *g = w->priv;

    if (g) {
        gfx_surface_free(g->content);
        gfx_surface_free(g->pic);
        kfree(g);
    }
}

static void gal_draw(struct gui_window *w)
{
    struct gal *g = w->priv;
    struct gfx_surface *s = w->surf;
    static const char *tabs[] = { "Controls", "Picture", "Scrolling" };
    struct gui_layout l;
    int x, y, cw, ch;

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, gui_th->win);
    if (!g)
        return;
    gui_layout_init(&l, 16, 14, w->cw - 32, w->ch - 28, true, 12);
    gui_layout_next(&l, 26, &x, &y, &cw, &ch);
    gui_segment(s, x, y, cw, tabs, 3, g->tab);
    gui_layout_next(&l, 0, &x, &y, &cw, &ch);
    if (g->tab == 0) {
        struct gui_layout rows;
        int rx, ry, rw, rh;
        gui_layout_init(&rows, x, y, cw, ch, true, 14);
        gui_layout_next(&rows, 22, &rx, &ry, &rw, &rh);
        gui_checkbox(s, rx, ry, "A check box", g->check);
        gui_layout_next(&rows, 22, &rx, &ry, &rw, &rh);
        gui_switch(s, rx, ry, g->sw);
        gfx_text(s, rx + 52, ry + 6, g->sw ? "Switch is on" : "Switch is off", gui_th->text, GFX_TRANSPARENT, 1);
        gui_layout_next(&rows, 22, &rx, &ry, &rw, &rh);
        gui_slider(s, rx, ry, MIN(rw, 300), g->slider, 100);
        gui_layout_next(&rows, 16, &rx, &ry, &rw, &rh);
        gui_progress(s, rx, ry, MIN(rw, 300), 10, g->slider, 100);
        gui_layout_next(&rows, 28, &rx, &ry, &rw, &rh);
        gui_button(s, rx, ry, 120, 28, "A button", false);
        gui_toolbar_button(s, rx + 130, ry + 2, 90, 24, "Toolbar", true);
    } else if (g->tab == 1) {
        gui_image(s, x, y, cw, MIN(ch, 220), g->pic);
        gfx_text(s, x, y + MIN(ch, 220) + 6, "gui_image() fits any picture into a box.", gui_th->dim, GFX_TRANSPARENT, 1);
        gfx_text(s, x, y + MIN(ch, 220) + 22, "Load files with gui_image_load(path): BMP, PNG, JPEG.", gui_th->dim, GFX_TRANSPARENT, 1);
    } else if (g->content) {
        g->sv.x = x; g->sv.y = y; g->sv.w = cw; g->sv.h = ch;
        gui_scroll_by(&g->sv, g->content, 0, 0);
        gfx_rect(s, x - 1, y - 1, cw + 2, ch + 2, gui_th->line);
        gui_scroll_draw(s, &g->sv, g->content);
    }
}

static void gal_click(struct gui_window *w, int px, int py, int b)
{
    struct gal *g = w->priv;
    struct gui_layout l;
    int x, y, cw, ch, t;
    (void)b;

    if (!g)
        return;
    gui_layout_init(&l, 16, 14, w->cw - 32, w->ch - 28, true, 12);
    gui_layout_next(&l, 26, &x, &y, &cw, &ch);
    if ((t = gui_tab_hit(px, py, x, y, cw, 3)) >= 0) {
        g->tab = t;
    } else if (g->tab == 0) {
        gui_layout_next(&l, 0, &x, &y, &cw, &ch);
        if (gui_checkbox_hit(px, py, x, y, "A check box"))
            g->check = !g->check;
        else if (gui_switch_hit(px, py, x, y + 36))
            g->sw = !g->sw;
        else if (gui_hit(px, py, x, y + 72, MIN(cw, 300), 22))
            g->slider = gui_slider_value(px, x, MIN(cw, 300), 100);
    } else if (g->tab == 2 && g->content) {
        gui_scroll_click(&g->sv, g->content, px, py);
    }
    gui_redraw();
}

static void gal_key(struct gui_window *w, int key, int mods)
{
    struct gal *g = w->priv;
    (void)mods;
    if (g && g->tab == 2 && g->content) {                /* arrow keys scroll both ways */
        if (key == KEY_DOWN) gui_scroll_by(&g->sv, g->content, 0, 38);
        else if (key == KEY_UP) gui_scroll_by(&g->sv, g->content, 0, -38);
        else if (key == KEY_RIGHT) gui_scroll_by(&g->sv, g->content, 38, 0);
        else if (key == KEY_LEFT) gui_scroll_by(&g->sv, g->content, -38, 0);
    }
    gui_redraw();
}

static void gal_icon(struct gfx_surface *s, int x, int y, int sz, uint32_t g, uint32_t tile)
{
#define P(f) ((sz * (f)) / 100)
    gfx_fill_round_rect(s, x + P(18), y + P(20), P(26), P(26), P(5), g);
    gfx_fill_round_rect(s, x + P(56), y + P(20), P(26), P(26), P(5), g);
    gfx_fill_round_rect(s, x + P(18), y + P(54), P(26), P(26), P(5), g);
    gfx_fill_circle(s, x + P(69), y + P(67), P(13), g);
    gfx_fill_circle(s, x + P(69), y + P(67), P(5), tile);
#undef P
}

static const struct gui_app gallery_app = {
    .name = "Gallery", .title = "Widget Gallery", .cw = 460, .ch = 330, .single = true, .dock = 85,
    .open = gal_open, .close = gal_close, .draw = gal_draw, .click = gal_click, .key = gal_key, .icon = gal_icon,
};
GUI_APP(gallery_app);
