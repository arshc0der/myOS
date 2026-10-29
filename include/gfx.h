/*
 * gfx.h - 2D graphics: surfaces, shapes, text.
 *
 * Everything draws into a "surface": a rectangle of 32-bit pixels in
 * memory (0x00RRGGBB). The screen has one (gfx_screen()); windows have
 * their own. Nothing appears until gfx_flush() copies it to the display.
 *
 *     struct gfx_surface *s = gfx_screen();
 *     gfx_fill_rect(s, 10, 10, 100, 50, GFX_RGB(255, 0, 0));
 *     gfx_text(s, 20, 20, "Hello", GFX_WHITE, GFX_TRANSPARENT, 1);
 *     gfx_flush_all();
 */
#ifndef GFX_H
#define GFX_H
#include "types.h"

#define GFX_RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define GFX_BLACK   GFX_RGB(0, 0, 0)
#define GFX_WHITE   GFX_RGB(255, 255, 255)
#define GFX_TRANSPARENT 0xFF000000u     /* "do not draw" for text backgrounds */

struct gfx_surface {
    uint32_t *px;
    int w, h;
    int stride;                         /* pixels per row */
    bool owned;                         /* pixel memory freed by gfx_surface_free */
};

/* ---- display ---- */
bool gfx_init(void);                    /* set up the boot framebuffer; false if none */
bool gfx_available(void);
struct gfx_surface *gfx_screen(void);   /* back buffer of the display */
int  gfx_width(void);
int  gfx_height(void);
void gfx_flush(int x, int y, int w, int h);     /* copy a region to the display */
void gfx_flush_all(void);
/* Read a pixel back from the real display memory (for tests/screenshots). */
uint32_t gfx_display_pixel(int x, int y);

/* ---- surfaces ---- */
struct gfx_surface *gfx_surface_create(int w, int h);
void gfx_surface_free(struct gfx_surface *s);
/* A surface for memory you own (tests): */
void gfx_surface_init(struct gfx_surface *s, uint32_t *mem, int w, int h);

/* ---- drawing (all clipped to the surface) ---- */
void gfx_pixel(struct gfx_surface *s, int x, int y, uint32_t c);
uint32_t gfx_get_pixel(const struct gfx_surface *s, int x, int y);
void gfx_fill_rect(struct gfx_surface *s, int x, int y, int w, int h, uint32_t c);
void gfx_rect(struct gfx_surface *s, int x, int y, int w, int h, uint32_t c);
void gfx_hline(struct gfx_surface *s, int x, int y, int w, uint32_t c);
void gfx_vline(struct gfx_surface *s, int x, int y, int h, uint32_t c);
void gfx_line(struct gfx_surface *s, int x0, int y0, int x1, int y1, uint32_t c);
void gfx_fill_circle(struct gfx_surface *s, int cx, int cy, int r, uint32_t c);
void gfx_fill_round_rect(struct gfx_surface *s, int x, int y, int w, int h, int r, uint32_t c);
void gfx_gradient_v(struct gfx_surface *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
/* Copy a region of src onto dst at (dx, dy). */
void gfx_blit(struct gfx_surface *dst, int dx, int dy,
              const struct gfx_surface *src, int sx, int sy, int w, int h);
/* Blend colour c over the existing pixels; alpha 0..255. */
void gfx_fill_rect_alpha(struct gfx_surface *s, int x, int y, int w, int h,
                         uint32_t c, int alpha);
uint32_t gfx_blend(uint32_t under, uint32_t over, int alpha);

/* ---- text (8x8 font, scale 1 = 8x8 pixels, 2 = 16x16 ...) ---- */
#define GFX_CHAR_W 8
#define GFX_CHAR_H 8
void gfx_char(struct gfx_surface *s, int x, int y, char ch, uint32_t fg, uint32_t bg, int scale);
void gfx_text(struct gfx_surface *s, int x, int y, const char *str, uint32_t fg, uint32_t bg, int scale);
int  gfx_text_width(const char *str, int scale);
/* Tall 8x16 variant (each font row drawn twice): used by the text console. */
void gfx_char_tall(struct gfx_surface *s, int x, int y, char ch, uint32_t fg, uint32_t bg);

extern const uint8_t font8x8[128][8];

/* ---- logo ---- */
/* The MyOS logo (concentric rounded squares and a dot), drawn at size `size`. */
void gfx_draw_logo(struct gfx_surface *s, int x, int y, int size);

#endif
