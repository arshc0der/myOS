/*
 * image.c - BMP, PNG and baseline JPEG decoders (see image.h).
 * Everything is written for clarity, not speed.
 */
#include "image.h"
#include "vfs.h"
#include "heap.h"
#include "kstring.h"
#include "kprintf.h"

#define MAX_FILE   (10u * 1024 * 1024)
#define MAX_PIXELS (1400u * 1024)               /* decoded size we keep (about 1400x1000) */

static uint32_t rgb_over(int r, int g, int b, int a, uint32_t bg)
{
    if (a >= 255)
        return GFX_RGB(r, g, b);
    int br = (bg >> 16) & 255, bgc = (bg >> 8) & 255, bb = bg & 255;
    return GFX_RGB((r * a + br * (255 - a)) / 255, (g * a + bgc * (255 - a)) / 255, (b * a + bb * (255 - a)) / 255);
}

static int shift_for(int w, int h)
{
    int sh = 0;
    while ((uint32_t)((w >> sh) + 1) * (uint32_t)((h >> sh) + 1) > MAX_PIXELS)
        sh++;
    return sh;
}

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

bool image_is_picture(const char *name)
{
    const char *d = strrchr(name, '.');
    if (!d)
        return false;
    char e[6];
    int i = 0;
    for (d++; *d && i < 5; d++, i++)
        e[i] = (*d >= 'A' && *d <= 'Z') ? (char)(*d + 32) : *d;
    e[i] = 0;
    return !strcmp(e, "bmp") || !strcmp(e, "png") || !strcmp(e, "jpg") || !strcmp(e, "jpeg");
}

/* ------------------------------------------------------------------ */
/* BMP                                                                 */
/* ------------------------------------------------------------------ */

static struct gfx_surface *load_bmp(const uint8_t *d, size_t n, uint32_t bg, const char **err)
{
    if (n < 54 || le32(d + 14) < 40) { *err = "not a supported BMP"; return NULL; }
    uint32_t off = le32(d + 10);
    int w = (int)le32(d + 18), h = (int)le32(d + 22);
    int bpp = d[28] | (d[29] << 8);
    uint32_t comp = le32(d + 30);
    bool flip = h > 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || (comp != 0 && comp != 3) || (bpp != 24 && bpp != 32 && bpp != 8)) {
        *err = "BMP: only 8, 24 and 32 bit uncompressed pictures";
        return NULL;
    }
    uint32_t pal = 14 + le32(d + 14);
    int sh = shift_for(w, h);
    int ow = (w + (1 << sh) - 1) >> sh, oh = (h + (1 << sh) - 1) >> sh;
    struct gfx_surface *s = gfx_surface_create(ow, oh);
    if (!s) { *err = "out of memory"; return NULL; }
    size_t stride = ((size_t)w * (size_t)bpp + 31) / 32 * 4;
    for (int oy = 0; oy < oh; oy++) {
        int y = oy << sh;
        int row = flip ? h - 1 - y : y;
        const uint8_t *r = d + off + (size_t)row * stride;
        if (off + (size_t)(row + 1) * stride > n) break;
        for (int ox = 0; ox < ow; ox++) {
            int x = ox << sh;
            uint32_t c;
            if (bpp == 8) {
                const uint8_t *e = d + pal + r[x] * 4u;
                c = GFX_RGB(e[2], e[1], e[0]);
            } else if (bpp == 24) {
                c = GFX_RGB(r[x * 3 + 2], r[x * 3 + 1], r[x * 3]);
            } else {
                c = rgb_over(r[x * 4 + 2], r[x * 4 + 1], r[x * 4], comp == 3 ? 255 : (r[x * 4 + 3] ? r[x * 4 + 3] : 255), bg);
            }
            s->px[oy * s->stride + ox] = c;
        }
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* Inflate (zlib) - the classic "puff" approach                        */
/* ------------------------------------------------------------------ */

struct inf {
    const uint8_t *in; size_t len, pos;
    uint32_t bits; int cnt;
    uint8_t *out; size_t cap, o;
    bool err;
};
struct huff { uint16_t count[16]; uint16_t sym[288]; };

static int getbits(struct inf *s, int need)
{
    uint32_t v = s->bits;
    while (s->cnt < need) {
        if (s->pos >= s->len) { s->err = true; return 0; }
        v |= (uint32_t)s->in[s->pos++] << s->cnt;
        s->cnt += 8;
    }
    s->bits = v >> need;
    s->cnt -= need;
    return (int)(v & ((1u << need) - 1));
}

static int decode(struct inf *s, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= getbits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first)
            return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static void build(struct huff *h, const uint8_t *len, int n)
{
    uint16_t offs[16];
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++)
        if (len[i]) h->sym[offs[len[i]]++] = (uint16_t)i;
}

static const uint16_t lbase[] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint16_t lext[]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t dbase[] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint16_t dext[]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void codes(struct inf *s, const struct huff *lc, const struct huff *dc)
{
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0) { s->err = true; return; }
        if (sym < 256) {
            if (s->o >= s->cap) { s->err = true; return; }
            s->out[s->o++] = (uint8_t)sym;
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) { s->err = true; return; }
            int len = lbase[sym] + getbits(s, lext[sym]);
            int ds = decode(s, dc);
            if (ds < 0 || ds >= 30) { s->err = true; return; }
            size_t dist = (size_t)dbase[ds] + (size_t)getbits(s, dext[ds]);
            if (s->err || dist > s->o || s->o + (size_t)len > s->cap) { s->err = true; return; }
            while (len--) { s->out[s->o] = s->out[s->o - dist]; s->o++; }
        }
    }
}

/* zlib stream -> out. Returns bytes produced or -1. */
static long inflate_zlib(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    struct inf s = { .in = in, .len = len, .pos = 2, .out = out, .cap = cap };
    static struct huff lc, dc;                  /* not reentrant; decoding runs in one task */
    int last;
    if (len < 2) return -1;
    do {
        last = getbits(&s, 1);
        int type = getbits(&s, 2);
        if (s.err) return -1;
        if (type == 0) {
            s.bits = 0; s.cnt = 0;
            if (s.pos + 4 > s.len) return -1;
            unsigned n = s.in[s.pos] | (s.in[s.pos + 1] << 8);
            s.pos += 4;
            if (s.pos + n > s.len || s.o + n > s.cap) return -1;
            memcpy(s.out + s.o, s.in + s.pos, n);
            s.pos += n; s.o += n;
        } else if (type == 1) {
            uint8_t l[288];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            build(&lc, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            build(&dc, l, 30);
            codes(&s, &lc, &dc);
        } else if (type == 2) {
            static const uint8_t order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            uint8_t l[320];
            int nlen = getbits(&s, 5) + 257, ndist = getbits(&s, 5) + 1, ncode = getbits(&s, 4) + 4;
            if (nlen > 286 || ndist > 30) return -1;
            int i;
            for (i = 0; i < 19; i++) l[i] = 0;
            for (i = 0; i < ncode; i++) l[order[i]] = (uint8_t)getbits(&s, 3);
            build(&lc, l, 19);
            uint8_t ll[320];
            i = 0;
            while (i < nlen + ndist) {
                int sym = decode(&s, &lc);
                if (sym < 0) return -1;
                if (sym < 16) ll[i++] = (uint8_t)sym;
                else {
                    int rep, v = 0;
                    if (sym == 16) { if (!i) return -1; v = ll[i - 1]; rep = 3 + getbits(&s, 2); }
                    else if (sym == 17) rep = 3 + getbits(&s, 3);
                    else rep = 11 + getbits(&s, 7);
                    if (i + rep > nlen + ndist) return -1;
                    while (rep--) ll[i++] = (uint8_t)v;
                }
            }
            build(&lc, ll, nlen);
            build(&dc, ll + nlen, ndist);
            codes(&s, &lc, &dc);
        } else return -1;
        if (s.err) return -1;
    } while (!last);
    return (long)s.o;
}

/* ------------------------------------------------------------------ */
/* PNG                                                                 */
/* ------------------------------------------------------------------ */

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

static struct gfx_surface *load_png(const uint8_t *d, size_t n, uint32_t bg, const char **err)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (n < 33 || memcmp(d, sig, 8)) { *err = "not a PNG"; return NULL; }
    int w = 0, h = 0, depth = 0, ctype = 0, inter = 0;
    uint8_t pal[256][3], trns[256];
    memset(trns, 255, sizeof trns);
    memset(pal, 0, sizeof pal);
    uint8_t *z = kmalloc(n);                    /* all IDAT data, joined */
    size_t zl = 0;
    if (!z) { *err = "out of memory"; return NULL; }
    for (size_t p = 8; p + 12 <= n;) {
        uint32_t cl = be32(d + p);
        const uint8_t *t = d + p + 4, *c = d + p + 8;
        if (p + 12 + cl > n) break;
        if (!memcmp(t, "IHDR", 4) && cl >= 13) {
            w = (int)be32(c); h = (int)be32(c + 4); depth = c[8]; ctype = c[9]; inter = c[12];
        } else if (!memcmp(t, "PLTE", 4)) {
            for (uint32_t i = 0; i < cl / 3 && i < 256; i++) { pal[i][0] = c[i * 3]; pal[i][1] = c[i * 3 + 1]; pal[i][2] = c[i * 3 + 2]; }
        } else if (!memcmp(t, "tRNS", 4) && ctype == 3) {
            for (uint32_t i = 0; i < cl && i < 256; i++) trns[i] = c[i];
        } else if (!memcmp(t, "IDAT", 4)) {
            memcpy(z + zl, c, cl); zl += cl;
        } else if (!memcmp(t, "IEND", 4)) break;
        p += 12 + cl;
    }
    int chans = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (w <= 0 || h <= 0 || !chans || inter || (depth != 8 && !(ctype == 3 && depth <= 8) && !(ctype == 0 && depth <= 8))) {
        kfree(z);
        *err = "PNG: only non-interlaced pictures with 8 bit colour are supported";
        return NULL;
    }
    size_t bits = (size_t)chans * (size_t)depth;
    size_t stride = ((size_t)w * bits + 7) / 8, bpp = (bits + 7) / 8;
    size_t total = (stride + 1) * (size_t)h;
    if (total > 20u * 1024 * 1024) { kfree(z); *err = "picture too large"; return NULL; }
    uint8_t *raw = kmalloc(total);
    if (!raw) { kfree(z); *err = "out of memory"; return NULL; }
    long got = inflate_zlib(z, zl, raw, total);
    kfree(z);
    if (got < (long)total) { kfree(raw); *err = "PNG data is damaged"; return NULL; }
    for (int y = 0; y < h; y++) {               /* undo the row filters */
        uint8_t *r = raw + (size_t)y * (stride + 1), f = r[0];
        uint8_t *cur = r + 1, *up = y ? r - stride : NULL;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = up ? up[i] : 0, c = (up && i >= bpp) ? up[i - bpp] : 0;
            switch (f) {
            case 1: cur[i] = (uint8_t)(cur[i] + a); break;
            case 2: cur[i] = (uint8_t)(cur[i] + b); break;
            case 3: cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1)); break;
            case 4: cur[i] = (uint8_t)(cur[i] + paeth(a, b, c)); break;
            }
        }
    }
    int sh = shift_for(w, h);
    int ow = (w + (1 << sh) - 1) >> sh, oh = (h + (1 << sh) - 1) >> sh;
    struct gfx_surface *s = gfx_surface_create(ow, oh);
    if (!s) { kfree(raw); *err = "out of memory"; return NULL; }
    for (int oy = 0; oy < oh; oy++) {
        const uint8_t *r = raw + (size_t)(oy << sh) * (stride + 1) + 1;
        for (int ox = 0; ox < ow; ox++) {
            int x = ox << sh, R, G, B, A = 255;
            if (ctype == 3 || ctype == 0) {
                int v;
                if (depth == 8) v = r[x];
                else { int per = 8 / depth, sft = 8 - depth * (x % per + 1); v = (r[x / per] >> sft) & ((1 << depth) - 1); }
                if (ctype == 3) { R = pal[v][0]; G = pal[v][1]; B = pal[v][2]; A = trns[v]; }
                else { if (depth < 8) v = v * 255 / ((1 << depth) - 1); R = G = B = v; }
            } else if (ctype == 2) { R = r[x * 3]; G = r[x * 3 + 1]; B = r[x * 3 + 2]; }
            else if (ctype == 4) { R = G = B = r[x * 2]; A = r[x * 2 + 1]; }
            else { R = r[x * 4]; G = r[x * 4 + 1]; B = r[x * 4 + 2]; A = r[x * 4 + 3]; }
            s->px[oy * s->stride + ox] = rgb_over(R, G, B, A, bg);
        }
    }
    kfree(raw);
    return s;
}

/* ------------------------------------------------------------------ */
/* JPEG (baseline and extended sequential, Huffman)                    */
/* ------------------------------------------------------------------ */

static const int idct_c[8][8] = {
    { 1448, 2009, 1892, 1703, 1448, 1138, 784, 400 },
    { 1448, 1703, 784, -400, -1448, -2009, -1892, -1138 },
    { 1448, 1138, -784, -2009, -1448, 400, 1892, 1703 },
    { 1448, 400, -1892, -1138, 1448, 1703, -784, -2009 },
    { 1448, -400, -1892, 1138, 1448, -1703, -784, 2009 },
    { 1448, -1138, -784, 2009, -1448, -400, 1892, -1703 },
    { 1448, -1703, 784, 400, -1448, 2009, -1892, 1138 },
    { 1448, -2009, 1892, -1703, 1448, -1138, 784, -400 },
};
static const uint8_t zigzag[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63 };

struct jh { uint8_t bits[17]; uint8_t val[256]; int mincode[17], maxcode[18], valptr[17]; bool ok; };
struct jcomp { int id, h, v, tq, td, ta, pred; uint8_t *plane; int pw, ph; };
struct jpg {
    const uint8_t *d; size_t n, pos;
    uint32_t buf; int cnt;
    uint16_t q[4][64];
    struct jh dc[4], ac[4];
    struct jcomp c[3];
    int nc, w, h, hmax, vmax, restart;
    bool eof;
};

static void jh_make(struct jh *t)
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = k;
        t->mincode[l] = code;
        code += t->bits[l];
        k += t->bits[l];
        t->maxcode[l] = t->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    t->ok = true;
}

static int jbit(struct jpg *j)
{
    if (j->cnt == 0) {
        if (j->pos >= j->n) { j->eof = true; return 0; }
        uint8_t b = j->d[j->pos++];
        if (b == 0xFF) {
            uint8_t b2 = j->pos < j->n ? j->d[j->pos] : 0;
            if (b2 == 0) j->pos++;
            else { j->pos--; j->eof = true; return 0; }    /* a marker: stay on it */
        }
        j->buf = b;
        j->cnt = 8;
    }
    j->cnt--;
    return (int)((j->buf >> j->cnt) & 1);
}

static int jbits(struct jpg *j, int n)
{
    int v = 0;
    while (n--) v = (v << 1) | jbit(j);
    return v;
}

static int jdecode(struct jpg *j, const struct jh *t)
{
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | jbit(j);
        if (t->maxcode[l] >= 0 && code <= t->maxcode[l] && code >= t->mincode[l])
            return t->val[t->valptr[l] + code - t->mincode[l]];
        if (j->eof) return 0;
    }
    j->eof = true;
    return 0;
}

static int jextend(int v, int t) { return t && v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }

static void jblock(struct jpg *j, struct jcomp *c, uint8_t *dst, int stride)
{
    int coef[64] = { 0 };
    int t = jdecode(j, &j->dc[c->td]);
    c->pred += jextend(jbits(j, t & 15), t & 15);
    coef[0] = c->pred * j->q[c->tq][0];
    for (int k = 1; k < 64 && !j->eof;) {
        int rs = jdecode(j, &j->ac[c->ta]), r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r == 15) { k += 16; continue; }
            break;
        }
        k += r;
        if (k > 63) break;
        coef[zigzag[k]] = jextend(jbits(j, s), s) * j->q[c->tq][k];
        k++;
    }
    int tmp[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int sum = 0;
            for (int u = 0; u < 8; u++) sum += idct_c[x][u] * coef[y * 8 + u];
            tmp[y * 8 + x] = sum >> 12;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int sum = 0;
            for (int v = 0; v < 8; v++) sum += idct_c[y][v] * tmp[v * 8 + x];
            sum = (sum >> 12) + 128;
            dst[y * stride + x] = (uint8_t)(sum < 0 ? 0 : sum > 255 ? 255 : sum);
        }
}

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static struct gfx_surface *load_jpeg(const uint8_t *d, size_t n, const char **err)
{
    static struct jpg j;                        /* big: keep it off the stack */
    memset(&j, 0, sizeof j);
    j.d = d; j.n = n; j.pos = 2;
    bool sof = false;
    while (j.pos + 4 <= n) {
        if (d[j.pos] != 0xFF) { j.pos++; continue; }
        int m = d[j.pos + 1];
        if (m == 0xFF || m == 0 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { j.pos += (m == 0xFF) ? 1 : 2; continue; }
        size_t len = (size_t)(d[j.pos + 2] << 8 | d[j.pos + 3]);
        const uint8_t *p = d + j.pos + 4;
        size_t seg_end = j.pos + 2 + len;
        if (seg_end > n) break;
        if (m == 0xC2 || m == 0xC9 || m == 0xCA) { *err = "progressive JPEG is not supported (save as baseline)"; return NULL; }
        if (m == 0xDB) {
            for (const uint8_t *e = p; e < d + seg_end;) {
                int pq = e[0] >> 4, tq = e[0] & 3;
                e++;
                for (int i = 0; i < 64; i++) { j.q[tq][i] = pq ? (uint16_t)(e[0] << 8 | e[1]) : e[0]; e += pq ? 2 : 1; }
            }
        } else if (m == 0xC0 || m == 0xC1) {
            j.h = p[1] << 8 | p[2]; j.w = p[3] << 8 | p[4]; j.nc = p[5];
            if (p[0] != 8 || (j.nc != 1 && j.nc != 3)) { *err = "JPEG: unsupported colour format"; return NULL; }
            for (int i = 0; i < j.nc; i++) {
                j.c[i].id = p[6 + i * 3]; j.c[i].h = p[7 + i * 3] >> 4; j.c[i].v = p[7 + i * 3] & 15; j.c[i].tq = p[8 + i * 3] & 3;
                if (j.c[i].h < 1 || j.c[i].h > 4 || j.c[i].v < 1 || j.c[i].v > 4) { *err = "JPEG: bad sampling"; return NULL; }
                if (j.c[i].h > j.hmax) j.hmax = j.c[i].h;
                if (j.c[i].v > j.vmax) j.vmax = j.c[i].v;
            }
            sof = true;
        } else if (m == 0xC4) {
            for (const uint8_t *e = p; e < d + seg_end;) {
                int tc = e[0] >> 4, th = e[0] & 3;
                struct jh *t = tc ? &j.ac[th] : &j.dc[th];
                int total = 0;
                for (int i = 1; i <= 16; i++) { t->bits[i] = e[i]; total += e[i]; }
                if (total > 256) { *err = "JPEG: bad table"; return NULL; }
                memcpy(t->val, e + 17, (size_t)total);
                jh_make(t);
                e += 17 + total;
            }
        } else if (m == 0xDD) {
            j.restart = p[0] << 8 | p[1];
        } else if (m == 0xDA) {
            if (!sof || j.w <= 0 || j.h <= 0) { *err = "JPEG: missing header"; return NULL; }
            int ns = p[0];
            if (ns != j.nc) { *err = "JPEG: unsupported scan layout"; return NULL; }
            for (int i = 0; i < ns; i++)
                for (int k = 0; k < j.nc; k++)
                    if (j.c[k].id == p[1 + i * 2]) { j.c[k].td = p[2 + i * 2] >> 4 & 3; j.c[k].ta = p[2 + i * 2] & 3; }
            j.pos = seg_end;
            break;
        }
        j.pos = seg_end;
    }
    if (!sof || j.pos >= n) { *err = "not a readable JPEG"; return NULL; }
    if ((uint32_t)j.w * (uint32_t)j.h > 12u * 1000 * 1000) { *err = "picture too large"; return NULL; }
    int mcux = (j.w + 8 * j.hmax - 1) / (8 * j.hmax), mcuy = (j.h + 8 * j.vmax - 1) / (8 * j.vmax);
    for (int i = 0; i < j.nc; i++) {
        j.c[i].pw = mcux * j.c[i].h * 8;
        j.c[i].ph = mcuy * j.c[i].v * 8;
        j.c[i].plane = kmalloc((size_t)j.c[i].pw * (size_t)j.c[i].ph);
        if (!j.c[i].plane) {
            for (int k = 0; k < i; k++) kfree(j.c[k].plane);
            *err = "out of memory";
            return NULL;
        }
    }
    int cnt = 0;
    for (int my = 0; my < mcuy && !j.eof; my++)
        for (int mx = 0; mx < mcux && !j.eof; mx++) {
            if (j.restart && cnt == j.restart) {            /* restart marker between groups */
                j.cnt = 0;
                while (j.pos + 1 < j.n && !(j.d[j.pos] == 0xFF && j.d[j.pos + 1] >= 0xD0 && j.d[j.pos + 1] <= 0xD7)) j.pos++;
                j.pos += 2;
                j.eof = false;
                for (int i = 0; i < j.nc; i++) j.c[i].pred = 0;
                cnt = 0;
            }
            cnt++;
            for (int i = 0; i < j.nc; i++)
                for (int by = 0; by < j.c[i].v; by++)
                    for (int bx = 0; bx < j.c[i].h; bx++) {
                        int px = (mx * j.c[i].h + bx) * 8, py = (my * j.c[i].v + by) * 8;
                        jblock(&j, &j.c[i], j.c[i].plane + (size_t)py * (size_t)j.c[i].pw + (size_t)px, j.c[i].pw);
                    }
        }
    int sh = shift_for(j.w, j.h);
    int ow = (j.w + (1 << sh) - 1) >> sh, oh = (j.h + (1 << sh) - 1) >> sh;
    struct gfx_surface *s = gfx_surface_create(ow, oh);
    if (s)
        for (int oy = 0; oy < oh; oy++)
            for (int ox = 0; ox < ow; ox++) {
                int x = ox << sh, y = oy << sh, v[3];
                for (int i = 0; i < j.nc; i++) {
                    int cx = x * j.c[i].h / j.hmax, cy = y * j.c[i].v / j.vmax;
                    v[i] = j.c[i].plane[(size_t)cy * (size_t)j.c[i].pw + (size_t)cx];
                }
                if (j.nc == 1)
                    s->px[oy * s->stride + ox] = GFX_RGB(v[0], v[0], v[0]);
                else {
                    int Y = v[0], cb = v[1] - 128, cr = v[2] - 128;
                    s->px[oy * s->stride + ox] = GFX_RGB(clamp8(Y + (91881 * cr >> 16)),
                                                         clamp8(Y - ((22554 * cb + 46802 * cr) >> 16)),
                                                         clamp8(Y + (116130 * cb >> 16)));
                }
            }
    for (int i = 0; i < j.nc; i++) kfree(j.c[i].plane);
    if (!s)
        *err = "out of memory";
    return s;
}

/* ------------------------------------------------------------------ */

struct gfx_surface *image_load(const char *path, uint32_t bg, const char **err)
{
    const char *dummy;
    struct vfs_stat st;
    if (!err) err = &dummy;
    *err = "cannot read the file";
    if (vfs_stat(path, &st) < 0 || st.type == VFS_DIR) return NULL;
    if (st.size < 16 || st.size > MAX_FILE) { *err = st.size < 16 ? "file is empty or too small" : "file is too large"; return NULL; }
    uint8_t *buf = kmalloc(st.size);
    if (!buf) { *err = "out of memory"; return NULL; }
    ssize_t n = vfs_read_file(path, buf, st.size);
    struct gfx_surface *s = NULL;
    if (n < 16) *err = "cannot read the file";
    else if (buf[0] == 'B' && buf[1] == 'M') s = load_bmp(buf, (size_t)n, bg, err);
    else if (buf[0] == 137 && buf[1] == 'P') s = load_png(buf, (size_t)n, bg, err);
    else if (buf[0] == 0xFF && buf[1] == 0xD8) s = load_jpeg(buf, (size_t)n, err);
    else *err = "unknown picture format (use BMP, PNG or JPG)";
    kfree(buf);
    return s;
}

struct gfx_surface *image_fit(const struct gfx_surface *src, int w, int h)
{
    if (w < 1 || h < 1 || src->w < 1 || src->h < 1) return NULL;
    int dw = w, dh = src->h * w / src->w;
    if (dh > h) { dh = h; dw = src->w * h / src->h; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    struct gfx_surface *o = gfx_surface_create(dw, dh);
    if (!o) return NULL;
    for (int y = 0; y < dh; y++) {
        int sy = (y * 2 + 1) * src->h / (dh * 2);
        int y0 = y * src->h / dh, y1 = (y + 1) * src->h / dh;
        if (y1 <= y0) y1 = y0 + 1;
        (void)sy;
        for (int x = 0; x < dw; x++) {
            int x0 = x * src->w / dw, x1 = (x + 1) * src->w / dw;
            if (x1 <= x0) x1 = x0 + 1;
            int step_y = (y1 - y0 + 3) / 4, step_x = (x1 - x0 + 3) / 4;     /* sample at most 4x4 pixels */
            unsigned r = 0, g = 0, b = 0, cnt = 0;
            for (int yy = y0; yy < y1 && yy < src->h; yy += step_y)
                for (int xx = x0; xx < x1 && xx < src->w; xx += step_x) {
                    uint32_t c = src->px[yy * src->stride + xx];
                    r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255; cnt++;
                }
            o->px[y * o->stride + x] = cnt ? GFX_RGB(r / cnt, g / cnt, b / cnt) : 0;
        }
    }
    return o;
}

int image_save_bmp(const struct gfx_surface *s, const char *path)
{
    size_t stride = ((size_t)s->w * 3 + 3) & ~(size_t)3, total = 54 + stride * (size_t)s->h;
    uint8_t *b = kzalloc(total);
    if (!b)
        return -12;
    b[0] = 'B'; b[1] = 'M';
    uint32_t v[] = { (uint32_t)total, 0, 54, 40, (uint32_t)s->w, (uint32_t)s->h };
    static const int at[] = { 2, 6, 10, 14, 18, 22 };
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 4; k++)
            b[at[i] + k] = (uint8_t)(v[i] >> (8 * k));
    b[26] = 1; b[28] = 24;
    for (int y = 0; y < s->h; y++) {
        uint8_t *r = b + 54 + stride * (size_t)(s->h - 1 - y);
        for (int x = 0; x < s->w; x++) {
            uint32_t c = s->px[y * s->stride + x];
            r[x * 3] = (uint8_t)c; r[x * 3 + 1] = (uint8_t)(c >> 8); r[x * 3 + 2] = (uint8_t)(c >> 16);
        }
    }
    int rc = vfs_write_file(path, b, total);
    kfree(b);
    return rc;
}
