/* myfont.c - see myfont.h. Freestanding C99, no heap, no libc calls. */
#include "myfont.h"

/* ---- little-endian readers (safe on any alignment) ---------------------- */
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }

/* header: magic[4] ver u16 flags u16 nsizes u16 nglyphs u16 cp_off u32 size_off u32
 *         kern_off u32 kern_count u32 upem u16 pad u16                       (32 bytes) */
enum { H_NSIZES = 8, H_NGLYPHS = 10, H_CPOFF = 12, H_SIZEOFF = 16, H_KERNOFF = 20, H_KERNCNT = 24, H_UPEM = 28 };
enum { SIZE_ENT = 12, GLYPH_ENT = 12, KERN_ENT = 8 };

int myfont_init(myfont_face *f, const void *blob, size_t len) {
    const uint8_t *b = (const uint8_t *)blob;
    if (!f || !b || len < 32) return -1;
    if (b[0] != 'M' || b[1] != 'F' || b[2] != 'B' || b[3] != '1') return -2;
    f->base = b; f->len = len;
    f->num_sizes = rd16(b + H_NSIZES);
    f->num_glyphs = rd16(b + H_NGLYPHS);
    f->units_per_em = rd16(b + H_UPEM);
    f->kern_count = rd32(b + H_KERNCNT);
    f->fallback = 0;
    if (f->num_sizes == 0 || f->num_glyphs < 95) return -3;
    if (rd32(b + H_CPOFF) + 4u * f->num_glyphs > len) return -4;
    if (rd32(b + H_SIZEOFF) + (uint32_t)SIZE_ENT * f->num_sizes > len) return -4;
    if (rd32(b + H_KERNOFF) + (uint32_t)KERN_ENT * f->kern_count > len) return -4;
    return 0;
}

void myfont_set_fallback(myfont_face *f, const myfont_face *fb) { f->fallback = fb; }

/* ---- size table --------------------------------------------------------- */
static const uint8_t *size_ent(const myfont_face *f, int i) {
    return f->base + rd32(f->base + H_SIZEOFF) + (uint32_t)i * SIZE_ENT;
}
int myfont_size_px(const myfont_face *f, int i)      { return rd16(size_ent(f, i)); }
int myfont_ascent(const myfont_face *f, int i)       { return rds16(size_ent(f, i) + 2); }
int myfont_descent(const myfont_face *f, int i)      { return rds16(size_ent(f, i) + 4); }
int myfont_line_height(const myfont_face *f, int i)  { return rd16(size_ent(f, i) + 6); }

int myfont_size_index(const myfont_face *f, int px) {
    int best = 0;
    for (int i = 0; i < f->num_sizes; i++) {
        if (myfont_size_px(f, i) <= px) best = i;
    }
    return best;
}

/* ---- glyph lookup ------------------------------------------------------- */
static int find_glyph(const myfont_face *f, uint32_t cp) {
    if (cp >= 0x20 && cp <= 0x7E) return (int)(cp - 0x20);   /* ASCII block is first, in order */
    const uint8_t *tab = f->base + rd32(f->base + H_CPOFF);
    int lo = 95, hi = (int)f->num_glyphs - 1;                 /* the rest is sorted */
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v = rd32(tab + 4 * mid);
        if (v == cp) return mid;
        if (v < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

typedef struct { const myfont_face *face; int size_idx; int glyph; } resolved;

/* Find the glyph in the face or its fallbacks; fall back to the same pixel size. */
static int resolve(const myfont_face *face, int size_idx, uint32_t cp, resolved *out) {
    int px = myfont_size_px(face, size_idx);
    for (const myfont_face *f = face; f; f = f->fallback) {
        int g = find_glyph(f, cp);
        if (g >= 0) {
            out->face = f; out->glyph = g;
            out->size_idx = (f == face) ? size_idx : myfont_size_index(f, px);
            return 1;
        }
    }
    return 0;
}

static const uint8_t *glyph_ent(const myfont_face *f, int size_idx, int g) {
    uint32_t tab = rd32(size_ent(f, size_idx) + 8);
    return f->base + tab + (uint32_t)g * GLYPH_ENT;
}

/* ---- UTF-8 -------------------------------------------------------------- */
static uint32_t next_cp(const char **ps) {
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t c = s[0];
    int n = 0;
    if (c < 0x80) { *ps += 1; return c; }
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { *ps += 1; return 0xFFFD; }
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *ps += i; return 0xFFFD; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *ps += n + 1;
    return c;
}

/* ---- kerning ------------------------------------------------------------ */
static int kern_px(const myfont_face *f, int size_idx, uint32_t l, uint32_t r) {
    if (!f->kern_count || l > 0xFFFF || r > 0xFFFF) return 0;
    const uint8_t *tab = f->base + rd32(f->base + H_KERNOFF);
    uint32_t key = (l << 16) | r, lo = 0, hi = f->kern_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1, v = rd32(tab + KERN_ENT * mid);
        if (v == key) {
            int units = rds16(tab + KERN_ENT * mid + 4);
            int px = myfont_size_px(f, size_idx);
            int num = units * px;                         /* round to nearest pixel */
            return (num >= 0 ? num + f->units_per_em / 2 : num - f->units_per_em / 2) / f->units_per_em;
        }
        if (v < key) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* ---- measuring and drawing --------------------------------------------- */
int myfont_text_width(const myfont_face *face, int size_idx, const char *s) {
    int w = 0; uint32_t prev = 0;
    while (*s) {
        uint32_t cp = next_cp(&s);
        resolved r;
        if (!resolve(face, size_idx, cp, &r) && !resolve(face, size_idx, '?', &r)) continue;
        if (prev && r.face == face) w += kern_px(face, size_idx, prev, cp);
        w += glyph_ent(r.face, r.size_idx, r.glyph)[8];
        prev = cp;
    }
    return w;
}

static inline uint32_t div255(uint32_t x) { x += 128; return (x + (x >> 8)) >> 8; }

static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a) {
    uint32_t ia = 255 - a;
    uint32_t r = div255(((src >> 16) & 255) * a + ((dst >> 16) & 255) * ia);
    uint32_t g = div255(((src >> 8) & 255) * a + ((dst >> 8) & 255) * ia);
    uint32_t b = div255((src & 255) * a + (dst & 255) * ia);
    return (r << 16) | (g << 8) | b;
}

int myfont_draw_text_clip(const myfont_surface *dst, const myfont_face *face, int size_idx,
                          int x, int base_y, uint32_t rgb, const char *s,
                          int cx0, int cy0, int cx1, int cy1) {
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > dst->width) cx1 = dst->width;
    if (cy1 > dst->height) cy1 = dst->height;
    uint32_t prev = 0;
    while (*s) {
        uint32_t cp = next_cp(&s);
        resolved r;
        if (!resolve(face, size_idx, cp, &r) && !resolve(face, size_idx, '?', &r)) continue;
        if (prev && r.face == face) x += kern_px(face, size_idx, prev, cp);
        const uint8_t *g = glyph_ent(r.face, r.size_idx, r.glyph);
        uint32_t off = rd32(g);
        int w = g[4], h = g[5], bx = (int8_t)g[6], by = (int8_t)g[7], adv = g[8];
        if (off && w && h) {
            const uint8_t *bits = r.face->base + off;
            int gx = x + bx, gy = base_y - by;
            for (int row = 0; row < h; row++) {
                int py = gy + row;
                if (py < cy0 || py >= cy1) continue;
                uint32_t *line = (uint32_t *)((uint8_t *)dst->pixels + (size_t)py * dst->pitch);
                const uint8_t *src = bits + row * w;
                for (int col = 0; col < w; col++) {
                    int px = gx + col;
                    uint32_t a = src[col];
                    if (!a || px < cx0 || px >= cx1) continue;
                    line[px] = (a == 255) ? rgb : blend(line[px], rgb, a);
                }
            }
        }
        x += adv;
        prev = cp;
    }
    return x;
}

int myfont_draw_text(const myfont_surface *dst, const myfont_face *face, int size_idx,
                     int x, int base_y, uint32_t rgb, const char *s) {
    return myfont_draw_text_clip(dst, face, size_idx, x, base_y, rgb, s, 0, 0, dst->width, dst->height);
}
