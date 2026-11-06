/* gfx.c - drawing primitives on 32-bit surfaces. */
#include "gfx.h"
#include "uifont.h"
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

static inline void fill32(uint32_t *d, uint32_t c, size_t n)
{
    __asm__ volatile ("cld; rep stosl" : "+D"(d), "+c"(n) : "a"(c) : "memory");
}

void gfx_fill_rect(struct gfx_surface *s, int x, int y, int w, int h, uint32_t c)
{
    int x0 = MAX(x, 0), y0 = MAX(y, 0);
    int x1 = MIN(x + w, s->w), y1 = MIN(y + h, s->h);

    if (x0 >= x1 || y0 >= y1)
        return;
    for (int j = y0; j < y1; j++)
        fill32(&s->px[j * s->stride + x0], c, (size_t)(x1 - x0));
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

/* coverage (0..255) of one corner pixel: D = 4 * (distance from the corner centre)^2, r = radius.
 * distance to the edge ~ (r^2 - d^2) / 2r, which is accurate within a pixel of the edge. */
static inline int edge_cov(int D, int r)
{
    int c = ((4 * r * r - D) * 32) / r + 128;
    return c < 0 ? 0 : c > 255 ? 255 : c;
}

static inline void put_cov(struct gfx_surface *s, int x, int y, uint32_t c, int cov)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h || cov <= 0)
        return;
    uint32_t *p = &s->px[y * s->stride + x];
    *p = cov >= 255 ? c : gfx_blend(*p, c, cov);
}

void gfx_fill_circle(struct gfx_surface *s, int cx, int cy, int r, uint32_t c)
{
    if (r <= 0) {
        gfx_pixel(s, cx, cy, c);
        return;
    }
    for (int y = -r - 1; y <= r + 1; y++)
        for (int x = -r - 1; x <= r + 1; x++) {
            int cov = edge_cov(4 * (x * x + y * y), r) ;
            /* the circle is centred on a pixel, so shift the edge half a pixel outward */
            cov = MIN(255, cov + 64);
            if (cov > 0 && (x * x + y * y) <= (r + 1) * (r + 1))
                put_cov(s, cx + x, cy + y, c, cov);
        }
}

void gfx_fill_round_rect(struct gfx_surface *s, int x, int y, int w, int h, int r, uint32_t c)
{
    if (w <= 0 || h <= 0)
        return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) {
        gfx_fill_rect(s, x, y, w, h, c);
        return;
    }
    gfx_fill_rect(s, x + r, y, w - 2 * r, h, c);
    gfx_fill_rect(s, x, y + r, r, h - 2 * r, c);
    gfx_fill_rect(s, x + w - r, y + r, r, h - 2 * r, c);
    /* four anti-aliased corners */
    for (int j = 0; j < r; j++) {
        int dy = 2 * r - 2 * j - 1;
        for (int i = 0; i < r; i++) {
            int dx = 2 * r - 2 * i - 1;
            int cov = edge_cov(dx * dx + dy * dy, r);
            if (cov <= 0)
                continue;
            put_cov(s, x + i, y + j, c, cov);
            put_cov(s, x + w - 1 - i, y + j, c, cov);
            put_cov(s, x + i, y + h - 1 - j, c, cov);
            put_cov(s, x + w - 1 - i, y + h - 1 - j, c, cov);
        }
    }
}

/* Integer blend of two 0x00RRGGBB colours: red+blue and green are computed together (no divisions). */
uint32_t gfx_blend(uint32_t under, uint32_t over, int a)
{
    if (a <= 0) return under;
    if (a >= 255) return over;
    uint32_t ia = (uint32_t)a + ((uint32_t)a >> 7);             /* 0..256 */
    uint32_t rb = (((over & 0xFF00FFu) * ia + (under & 0xFF00FFu) * (256 - ia)) >> 8) & 0xFF00FFu;
    uint32_t g = (((over & 0x00FF00u) * ia + (under & 0x00FF00u) * (256 - ia)) >> 8) & 0x00FF00u;
    return rb | g;
}

void gfx_fill_rect_alpha(struct gfx_surface *s, int x, int y, int w, int h,
                         uint32_t c, int alpha)
{
    int x0 = MAX(x, 0), y0 = MAX(y, 0);
    int x1 = MIN(x + w, s->w), y1 = MIN(y + h, s->h);

    if (alpha <= 0)
        return;
    if (alpha >= 255) {
        gfx_fill_rect(s, x, y, w, h, c);
        return;
    }
    uint32_t ia = (uint32_t)alpha + ((uint32_t)alpha >> 7), inv = 256 - ia;
    uint32_t crb = (c & 0xFF00FFu) * ia, cg = (c & 0x00FF00u) * ia;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = &s->px[j * s->stride];
        for (int i = x0; i < x1; i++) {
            uint32_t u = row[i];
            row[i] = ((((u & 0xFF00FFu) * inv + crb) >> 8) & 0xFF00FFu) | ((((u & 0x00FF00u) * inv + cg) >> 8) & 0x00FF00u);
        }
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
                &src->px[(sy + j) * src->stride + sx], (size_t)w * 4);          /* word copy when rows do not overlap */
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

/* All proportional UI text goes through myFont. `scale` keeps its old meaning (1 = body text, 2 = headline ...). */
static void text_style(int scale, int *px, int *weight, int *top)
{
    switch (scale < 1 ? 1 : scale) {
    case 1:  *px = 14; *weight = UI_REGULAR; *top = 0;  break;
    case 2:  *px = 20; *weight = UI_SEMIBOLD; *top = 0; break;
    case 3:  *px = 24; *weight = UI_BOLD; *top = 0;     break;
    default: *px = 32; *weight = UI_BOLD; *top = 0;     break;
    }
}

void gfx_text(struct gfx_surface *s, int x, int y, const char *str, uint32_t fg,
              uint32_t bg, int scale)
{
    int px, wt, top;
    text_style(scale, &px, &wt, &top);
    if (bg != GFX_TRANSPARENT)
        gfx_fill_rect(s, x, y - 2, ui_text_width(str, px, wt), 8 * (scale < 1 ? 1 : scale) + 4, bg);
    /* centre the glyphs vertically in the old 8 * scale pixel cell */
    int cell = 8 * (scale < 1 ? 1 : scale);
    int asc = ui_text_ascent(px, wt);
    int base = y + (cell + asc * 7 / 10) / 2 + 1;       /* cap height is about 70% of the ascent */
    (void)top;
    ui_text(s, x, base - asc, str, fg, px, wt);
}

int gfx_text_width(const char *str, int scale)
{
    int px, wt, top;
    text_style(scale, &px, &wt, &top);
    return ui_text_width(str, px, wt);
}

/* ---------------- logo ---------------- */

void gfx_draw_logo(struct gfx_surface *s, int x, int y, int size)
{
    /* black rounded tile, nested white/black rounded squares, centre dot */
    uint32_t k = GFX_RGB(18, 18, 20), w = GFX_RGB(250, 250, 252);
    int r = size / 5;
    gfx_fill_round_rect(s, x, y, size, size, r, k);
    int q = size / 6;
    gfx_fill_round_rect(s, x + q, y + q, size - 2 * q, size - 2 * q, MAX(r / 2, 1), w);
    int q2 = q + MAX(size / 14, 1) + 1;
    gfx_fill_round_rect(s, x + q2, y + q2, size - 2 * q2, size - 2 * q2, MAX(r / 3, 1), k);
    int q3 = q2 + MAX(size / 14, 1) + 1;
    gfx_fill_round_rect(s, x + q3, y + q3, size - 2 * q3, size - 2 * q3, MAX(r / 4, 1), w);
    gfx_fill_circle(s, x + size / 2, y + size / 2, MAX(size / 14, 2), k);
}
