/*
 * myfont.h - freestanding text renderer for myFont bitmap fonts (.mfb)
 *
 * - no libc, no malloc: a face is a view over a read-only memory blob
 * - ASCII glyph lookup is O(1); other code points use a binary search
 * - anti-aliased 8-bit coverage blended into a 32-bit 0x00RRGGBB framebuffer
 * - font fallback chain (e.g. myFont Regular -> myFont Mono -> NULL)
 * - kerning from the font's GPOS table (baked into the file)
 *
 * Build: add myfont.c to the kernel; needs only <stdint.h> and <stddef.h>.
 */
#ifndef MYFONT_H
#define MYFONT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct myfont_surface {
    uint32_t *pixels;       /* 0x00RRGGBB */
    int       width;
    int       height;
    int       pitch;        /* bytes per row */
} myfont_surface;

typedef struct myfont_face {
    const uint8_t *base;    /* start of the .mfb blob */
    size_t         len;
    uint16_t       num_sizes;
    uint16_t       num_glyphs;
    uint16_t       units_per_em;
    uint32_t       kern_count;
    const struct myfont_face *fallback;   /* NULL or next face to try */
} myfont_face;

/* Validate and attach a blob. Returns 0 on success, negative on error. */
int  myfont_init(myfont_face *face, const void *blob, size_t len);
void myfont_set_fallback(myfont_face *face, const myfont_face *fallback);

/* Index of the baked size closest to `px` (prefers the largest size <= px). */
int  myfont_size_index(const myfont_face *face, int px);
int  myfont_size_px(const myfont_face *face, int size_idx);
int  myfont_ascent(const myfont_face *face, int size_idx);
int  myfont_descent(const myfont_face *face, int size_idx);      /* positive number */
int  myfont_line_height(const myfont_face *face, int size_idx);

/* Pixel width of a UTF-8 string (stops at '\0'), including kerning. */
int  myfont_text_width(const myfont_face *face, int size_idx, const char *utf8);

/* Draw at (x, baseline_y). Returns the x position after the last glyph. */
int  myfont_draw_text(const myfont_surface *dst, const myfont_face *face, int size_idx,
                      int x, int baseline_y, uint32_t rgb, const char *utf8);

/* Same, clipped to a rectangle (x0,y0)-(x1,y1) exclusive. */
int  myfont_draw_text_clip(const myfont_surface *dst, const myfont_face *face, int size_idx,
                           int x, int baseline_y, uint32_t rgb, const char *utf8,
                           int clip_x0, int clip_y0, int clip_x1, int clip_y1);

#ifdef __cplusplus
}
#endif
#endif /* MYFONT_H */
