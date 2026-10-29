/* gfx.c - drawing primitives on 32-bit surfaces. */
#include "gfx.h"
#include "heap.h"
#include "pmm.h"
#include "kstring.h"

static inline int iabs(int v) { return v < 0 ? -v : v; }

void gfx_surface_init(struct gfx_surface *s, uint32_t *mem, int w, int h)
{
    s->px = mem;
    s->w = w;
    s->h = h;
    s->stride = w;
    s->owned = false;
}

struct gfx_surface *gfx_surface_create(int w, int h)
{
    if (w <= 0 || h <= 0)
        return NULL;
    struct gfx_surface *s = kzalloc(sizeof *s);
    size_t bytes = (size_t)w * (size_t)h * 4;

    if (!s)
        return NULL;
    s->px = kzalloc(bytes);
    if (!s->px) {
        kfree(s);
        return NULL;
    }
    s->w = w;
    s->h = h;
    s->stride = w;
    s->owned = true;
    return s;
}

void gfx_surface_free(struct gfx_surface *s)
{
    if (!s)
        return;
    if (s->owned)
        kfree(s->px);
    kfree(s);
}

void gfx_pixel(struct gfx_surface *s, int x, int y, uint32_t c)
{
    if (x >= 0 && y >= 0 && x < s->w && y < s->h)
        s->px[y * s->stride + x] = c;
}

uint32_t gfx_get_pixel(const struct gfx_surface *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h)
        return 0;
    return s->px[y * s->stride + x];
}

void gfx_fill_rect(struct gfx_surface *s, int x, int y, int w, int h, uint32_t c)
{
    int x0 = MAX(x, 0), y0 = MAX(y, 0);
    int x1 = MIN(x + w, s->w), y1 = MIN(y + h, s->h);

    if (x0 >= x1 || y0 >= y1)
        return;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = &s->px[j * s->stride + x0];
        for (int i = 0; i < x1 - x0; i++)
            row[i] = c;
    }
}

void gfx_hline(struct gfx_surface *s, int x, int y, int w, uint32_t c)
{
    gfx_fill_rect(s, x, y, w, 1, c);
}

void gfx_vline(struct gfx_surface *s, int x, int y, int h, uint32_t c)
{
    gfx_fill_rect(s, x, y, 1, h, c);
}

void gfx_rect(struct gfx_surface *s, int x, int y, int w, int h, uint32_t c)
{
    if (w <= 0 || h <= 0)
        return;
    gfx_hline(s, x, y, w, c);
    gfx_hline(s, x, y + h - 1, w, c);
    gfx_vline(s, x, y, h, c);
    gfx_vline(s, x + w - 1, y, h, c);
}

void gfx_line(struct gfx_surface *s, int x0, int y0, int x1, int y1, uint32_t c)
{
    int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        gfx_pixel(s, x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_fill_circle(struct gfx_surface *s, int cx, int cy, int r, uint32_t c)
{
    for (int y = -r; y <= r; y++) {
        int w = 0;
        while (w * w + y * y <= r * r + r)
            w++;
        w--;
        gfx_hline(s, cx - w, cy + y, 2 * w + 1, c);
    }
}

void gfx_fill_round_rect(struct gfx_surface *s, int x, int y, int w, int h, int r, uint32_t c)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) {
        gfx_fill_rect(s, x, y, w, h, c);
        return;
    }
    gfx_fill_rect(s, x + r, y, w - 2 * r, h, c);
    gfx_fill_rect(s, x, y + r, r, h - 2 * r, c);
    gfx_fill_rect(s, x + w - r, y + r, r, h - 2 * r, c);
    /* four corners */
    for (int j = 0; j < r; j++) {
        int dy = r - 1 - j;
        int ww = 0;
        while ((ww + 1) * (ww + 1) + dy * dy <= r * r)
            ww++;
        int inset = r - ww;
        gfx_hline(s, x + inset, y + j, r - inset, c);
        gfx_hline(s, x + w - r, y + j, r - inset, c);
        gfx_hline(s, x + inset, y + h - 1 - j, r - inset, c);
        gfx_hline(s, x + w - r, y + h - 1 - j, r - inset, c);
    }
}

uint32_t gfx_blend(uint32_t under, uint32_t over, int a)
{
    int ur = (under >> 16) & 255, ug = (under >> 8) & 255, ub = under & 255;
    int or_ = (over >> 16) & 255, og = (over >> 8) & 255, ob = over & 255;

    return GFX_RGB((or_ * a + ur * (255 - a)) / 255,
                   (og * a + ug * (255 - a)) / 255,
                   (ob * a + ub * (255 - a)) / 255);
}

void gfx_fill_rect_alpha(struct gfx_surface *s, int x, int y, int w, int h,
                         uint32_t c, int alpha)
{
    int x0 = MAX(x, 0), y0 = MAX(y, 0);
    int x1 = MIN(x + w, s->w), y1 = MIN(y + h, s->h);

    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) {
            uint32_t *p = &s->px[j * s->stride + i];
            *p = gfx_blend(*p, c, alpha);
        }
}

void gfx_gradient_v(struct gfx_surface *s, int x, int y, int w, int h,
                    uint32_t top, uint32_t bottom)
{
    for (int j = 0; j < h; j++) {
        int a = h > 1 ? j * 255 / (h - 1) : 0;
        gfx_fill_rect(s, x, y + j, w, 1, gfx_blend(top, bottom, a));
    }
}

void gfx_blit(struct gfx_surface *dst, int dx, int dy,
              const struct gfx_surface *src, int sx, int sy, int w, int h)
{
    /* clip against the source */
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    w = MIN(w, src->w - sx);
    h = MIN(h, src->h - sy);
    /* clip against the destination */
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    w = MIN(w, dst->w - dx);
    h = MIN(h, dst->h - dy);
    if (w <= 0 || h <= 0)
        return;
    for (int j = 0; j < h; j++)
        memmove(&dst->px[(dy + j) * dst->stride + dx],
                &src->px[(sy + j) * src->stride + sx], (size_t)w * 4);
}

/* ---------------- text ---------------- */

void gfx_char(struct gfx_surface *s, int x, int y, char ch, uint32_t fg,
              uint32_t bg, int scale)
{
    const uint8_t *g = font8x8[(uint8_t)ch & 0x7F];

    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++) {
            bool on = (g[row] >> col) & 1;
            uint32_t c = on ? fg : bg;
            if (c == GFX_TRANSPARENT)
                continue;
            if (scale == 1)
                gfx_pixel(s, x + col, y + row, c);
            else
                gfx_fill_rect(s, x + col * scale, y + row * scale, scale, scale, c);
        }
}

void gfx_char_tall(struct gfx_surface *s, int x, int y, char ch, uint32_t fg, uint32_t bg)
{
    const uint8_t *g = font8x8[(uint8_t)ch & 0x7F];

    for (int row = 0; row < 16; row++) {
        uint8_t bits = g[row / 2];
        for (int col = 0; col < 8; col++) {
            uint32_t c = ((bits >> col) & 1) ? fg : bg;
            if (c != GFX_TRANSPARENT)
                gfx_pixel(s, x + col, y + row, c);
        }
    }
}

void gfx_text(struct gfx_surface *s, int x, int y, const char *str, uint32_t fg,
              uint32_t bg, int scale)
{
    for (; *str; str++, x += 8 * scale) {
        if (*str == '\n')
            continue;
        gfx_char(s, x, y, *str, fg, bg, scale);
    }
}

int gfx_text_width(const char *str, int scale)
{
    return (int)strlen(str) * 8 * scale;
}

/* ---------------- logo ---------------- */

void gfx_draw_logo(struct gfx_surface *s, int x, int y, int size)
{
    /* rounded blue tile with three nested rounded squares and a centre dot */
    int r = size / 5;
    gfx_fill_round_rect(s, x, y, size, size, r, GFX_RGB(24, 90, 220));
    gfx_gradient_v(s, x + r / 2, y + 2, size - r, size / 3, GFX_RGB(70, 140, 250), GFX_RGB(24, 90, 220));
    gfx_fill_round_rect(s, x, y + size / 2, size, size - size / 2, r, GFX_RGB(24, 90, 220));
    gfx_fill_round_rect(s, x + size / 2 - size / 8, y + size / 2 - size / 8, size / 4, size / 4, 1, GFX_RGB(24, 90, 220));

    int q = size / 8;
    gfx_fill_round_rect(s, x + q, y + q, size - 2 * q, size - 2 * q, r / 2, GFX_RGB(240, 244, 252));
    gfx_fill_round_rect(s, x + q + q / 2 + 1, y + q + q / 2 + 1, size - 2 * q - q - 2,
                        size - 2 * q - q - 2, r / 3, GFX_RGB(30, 100, 230));
    int q2 = q * 2 + q / 2;
    gfx_fill_round_rect(s, x + q2, y + q2, size - 2 * q2, size - 2 * q2, r / 3, GFX_RGB(240, 244, 252));
    int q3 = q2 + q / 2 + 1;
    gfx_fill_round_rect(s, x + q3, y + q3, size - 2 * q3, size - 2 * q3, r / 4, GFX_RGB(30, 100, 230));
    gfx_fill_circle(s, x + size / 2, y + size / 2, MAX(size / 14, 2), GFX_RGB(240, 244, 252));
}
