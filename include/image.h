/*
 * image.h - load pictures: BMP (8/24/32 bit), PNG (non-interlaced, 8 bit) and
 * baseline JPEG. Big pictures are shrunk while loading so they fit in memory.
 *
 *     const char *err;
 *     struct gfx_surface *pic = image_load("/Users/arsh/Pictures/cat.jpg", GFX_WHITE, &err);
 *     if (!pic) kprintf("cannot open: %s\n", err);
 *
 * Transparent areas are blended with `bg`. Free the result with gfx_surface_free().
 */
#ifndef IMAGE_H
#define IMAGE_H
#include "gfx.h"

struct gfx_surface *image_load(const char *path, uint32_t bg, const char **err);
/* Scale `src` to fit inside w x h (keeps the shape). Returns a new surface. */
struct gfx_surface *image_fit(const struct gfx_surface *src, int w, int h);
/* True when the file name ends in a picture extension (.bmp .png .jpg .jpeg). */
bool image_is_picture(const char *name);
/* Decode a baseline JPEG that is already in memory (webcam MJPEG frames). NULL on failure. */
struct gfx_surface *image_from_jpeg(const uint8_t *data, size_t n);
/* Write a surface as a 24 bit BMP file. Returns 0 or -errno. */
int image_save_bmp(const struct gfx_surface *s, const char *path);
#endif
