/* widgets2.c - layout, scroll bars, check boxes, switches, sliders, progress bars, pictures, scroll views. */
#include "guiapi.h"
#include "image.h"
#include "kstring.h"

void gui_layout_init(struct gui_layout *l, int x, int y, int w, int h, bool vertical, int gap)
{
    l->x = x; l->y = y; l->w = w; l->h = h; l->gap = gap; l->pos = 0; l->vertical = vertical;
}

void gui_layout_next(struct gui_layout *l, int size, int *x, int *y, int *w, int *h)
{
    int total = l->vertical ? l->h : l->w;
    int left = total - l->pos;

    if (size <= 0 || size > left)
        size = MAX(left, 0);
    if (l->vertical) {
        *x = l->x; *y = l->y + l->pos; *w = l->w; *h = size;
    } else {
        *x = l->x + l->pos; *y = l->y; *w = size; *h = l->h;
    }
    l->pos += size + l->gap;
}

void gui_hscrollbar(struct gfx_surface *s, int x, int y, int w, int total, int visible, int left)
{
    if (total <= visible || total <= 0)
        return;
    int tw = MAX(w * visible / total, 20);
    int tx = x + (w - tw) * left / MAX(total - visible, 1);
    gfx_fill_round_rect(s, tx, y, tw, 5, 2, gui_th->dim);
}

void gui_checkbox(struct gfx_surface *s, int x, int y, const char *label, bool checked)
{
    gfx_fill_round_rect(s, x, y, 16, 16, 4, checked ? gui_th->accent : gui_th->line);
    if (!checked)
        gfx_fill_round_rect(s, x + 1, y + 1, 14, 14, 3, gui_th->win);
    else {
        gfx_line(s, x + 4, y + 8, x + 7, y + 11, gui_th->accent_fg);
        gfx_line(s, x + 7, y + 11, x + 12, y + 4, gui_th->accent_fg);
        gfx_line(s, x + 4, y + 9, x + 7, y + 12, gui_th->accent_fg);
        gfx_line(s, x + 7, y + 12, x + 12, y + 5, gui_th->accent_fg);
    }
    gfx_text(s, x + 24, y + 4, label, gui_th->text, GFX_TRANSPARENT, 1);
}

bool gui_checkbox_hit(int px, int py, int x, int y, const char *label)
{
    return gui_hit(px, py, x, y, 24 + gfx_text_width(label, 1), 16);
}

void gui_switch(struct gfx_surface *s, int x, int y, bool on)
{
    gfx_fill_round_rect(s, x, y, 40, 22, 11, on ? gui_th->accent : gui_th->line);
    gfx_fill_circle(s, on ? x + 29 : x + 11, y + 11, 9, GFX_WHITE);
}

bool gui_switch_hit(int px, int py, int x, int y) { return gui_hit(px, py, x, y, 40, 22); }

void gui_slider(struct gfx_surface *s, int x, int y, int w, int value, int max)
{
    int kx = x + (max > 0 ? (w - 16) * value / max : 0);

    gfx_fill_round_rect(s, x, y + 7, w, 4, 2, gui_th->line);
    gfx_fill_round_rect(s, x, y + 7, kx - x + 8, 4, 2, gui_th->accent);
    gfx_fill_circle(s, kx + 8, y + 9, 8, GFX_WHITE);
    gfx_fill_circle(s, kx + 8, y + 9, 6, gui_th->accent);
}

int gui_slider_value(int px, int x, int w, int max)
{
    int v = (px - x - 8) * max / MAX(w - 16, 1);
    return v < 0 ? 0 : v > max ? max : v;
}

void gui_progress(struct gfx_surface *s, int x, int y, int w, int h, int value, int max)
{
    gfx_fill_round_rect(s, x, y, w, h, h / 2, gui_th->line);
    if (max > 0 && value > 0) {
        int fw = MIN(w, MAX(h, w * value / max));
        gfx_fill_round_rect(s, x, y, fw, h, h / 2, gui_th->accent);
    }
}

int gui_tab_hit(int px, int py, int x, int y, int w, int n)
{
    if (px < x || px >= x + w || py < y || py >= y + 26 || n <= 0)
        return -1;
    return (px - x) * n / w;
}

struct gfx_surface *gui_image_load(const char *path)
{
    return image_load(path, gui_th->win, NULL);
}

void gui_image(struct gfx_surface *s, int x, int y, int w, int h, const struct gfx_surface *img)
{
    if (!img)
        return;
    struct gfx_surface *f = image_fit(img, w, h);
    if (!f)
        return;
    gfx_blit(s, x + (w - f->w) / 2, y + (h - f->h) / 2, f, 0, 0, f->w, f->h);
    gfx_surface_free(f);
}

/* ---- scroll view ---- */

static bool need_v(const struct gui_scroll *sv, const struct gfx_surface *c) { return c->h > sv->h; }
static bool need_h(const struct gui_scroll *sv, const struct gfx_surface *c) { return c->w > sv->w; }

void gui_scroll_by(struct gui_scroll *sv, const struct gfx_surface *c, int dx, int dy)
{
    sv->ox = MAX(0, MIN(c->w - sv->w, sv->ox + dx));
    sv->oy = MAX(0, MIN(c->h - sv->h, sv->oy + dy));
}

void gui_scroll_draw(struct gfx_surface *s, const struct gui_scroll *sv, const struct gfx_surface *c)
{
    int vw = MIN(sv->w, c->w - sv->ox), vh = MIN(sv->h, c->h - sv->oy);

    if (vw > 0 && vh > 0)
        gfx_blit(s, sv->x, sv->y, c, sv->ox, sv->oy, vw, vh);
    if (need_v(sv, c))
        gui_scrollbar(s, sv->x + sv->w - 7, sv->y, sv->h, c->h, sv->h, sv->oy);
    if (need_h(sv, c))
        gui_hscrollbar(s, sv->x, sv->y + sv->h - 7, sv->w, c->w, sv->w, sv->ox);
}

/* A click on the bar area jumps to that position. */
bool gui_scroll_click(struct gui_scroll *sv, const struct gfx_surface *c, int px, int py)
{
    if (need_v(sv, c) && gui_hit(px, py, sv->x + sv->w - 12, sv->y, 12, sv->h)) {
        sv->oy = MAX(0, MIN(c->h - sv->h, (py - sv->y) * c->h / sv->h - sv->h / 2));
        return true;
    }
    if (need_h(sv, c) && gui_hit(px, py, sv->x, sv->y + sv->h - 12, sv->w, 12)) {
        sv->ox = MAX(0, MIN(c->w - sv->w, (px - sv->x) * c->w / sv->w - sv->w / 2));
        return true;
    }
    return false;
}
