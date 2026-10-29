/*
 * framebuffer.c - the display.
 *
 * GRUB (or UEFI) gives us a "framebuffer": a block of memory where each
 * pixel on screen is a few bytes. We draw into a normal RAM copy (the
 * back buffer) and copy the changed part to the real framebuffer with
 * gfx_flush(). This avoids flicker and is faster than drawing straight
 * into slow display memory.
 */
#include "gfx.h"
#include "multiboot.h"
#include "pmm.h"
#include "vmm.h"
#include "kstring.h"
#include "log.h"

static struct gfx_surface screen;
static bool ready;
static uint8_t *fb;                     /* real framebuffer */
static uint32_t fb_pitch, fb_bpp;
static uint8_t rpos, rsize, gpos, gsize, bpos, bsize;
static bool fast32;                     /* 32 bpp, standard XRGB: plain copy */

bool gfx_available(void) { return ready; }
struct gfx_surface *gfx_screen(void) { return &screen; }
int gfx_width(void) { return screen.w; }
int gfx_height(void) { return screen.h; }

bool gfx_init(void)
{
    const struct boot_fb *f = &boot_info.fb;

    if (ready)
        return true;
    if (!f->present || f->is_text || f->width == 0 || f->height == 0)
        return false;
    if ((f->addr >> 32) != 0) {
        log_warn("gfx", "framebuffer is above 4 GiB; cannot map it in 32-bit mode");
        return false;
    }
    if (f->bpp != 32 && f->bpp != 24 && f->bpp != 16) {
        log_warn("gfx", "unsupported pixel depth: %u bpp", f->bpp);
        return false;
    }

    uint32_t bytes = f->pitch * f->height;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    paddr_t back = pmm_alloc_contiguous(((f->width * f->height * 4) + PAGE_SIZE - 1) / PAGE_SIZE);

    if (!back) {
        log_warn("gfx", "not enough memory for the back buffer");
        return false;
    }
    if (vmm_enabled())
        vmm_map_range((vaddr_t)f->addr, (paddr_t)f->addr, pages * PAGE_SIZE,
                      VMM_WRITE | VMM_NOCACHE);

    fb = (uint8_t *)(uint32_t)f->addr;
    fb_pitch = f->pitch;
    fb_bpp = f->bpp;
    rpos = f->red_pos;   rsize = f->red_size;
    gpos = f->green_pos; gsize = f->green_size;
    bpos = f->blue_pos;  bsize = f->blue_size;
    if (rsize == 0 && gsize == 0 && bsize == 0) {       /* no info: assume XRGB */
        rpos = 16; rsize = 8; gpos = 8; gsize = 8; bpos = 0; bsize = 8;
        if (fb_bpp == 16) { rpos = 11; rsize = 5; gpos = 5; gsize = 6; bpos = 0; bsize = 5; }
    }
    fast32 = fb_bpp == 32 && rpos == 16 && gpos == 8 && bpos == 0 &&
             rsize == 8 && gsize == 8 && bsize == 8;

    memset((void *)back, 0, (size_t)f->width * f->height * 4);
    gfx_surface_init(&screen, (uint32_t *)back, (int)f->width, (int)f->height);
    ready = true;
    log_info("gfx", "framebuffer %ux%u, %u bpp at 0x%08x (%s)", f->width, f->height,
             f->bpp, (uint32_t)f->addr, fast32 ? "direct copy" : "converted");
    return true;
}

static inline uint32_t pack(uint32_t c)
{
    uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;

    return ((r >> (8 - rsize)) << rpos) | ((g >> (8 - gsize)) << gpos) |
           ((b >> (8 - bsize)) << bpos);
}

void gfx_flush(int x, int y, int w, int h)
{
    if (!ready)
        return;
    int x0 = MAX(x, 0), y0 = MAX(y, 0);
    int x1 = MIN(x + w, screen.w), y1 = MIN(y + h, screen.h);

    if (x0 >= x1 || y0 >= y1)
        return;
    for (int j = y0; j < y1; j++) {
        const uint32_t *src = &screen.px[j * screen.stride + x0];
        uint8_t *dst = fb + (size_t)j * fb_pitch + (size_t)x0 * (fb_bpp / 8);
        int n = x1 - x0;

        if (fast32) {
            memcpy(dst, src, (size_t)n * 4);
        } else if (fb_bpp == 32) {
            uint32_t *d = (uint32_t *)dst;
            for (int i = 0; i < n; i++)
                d[i] = pack(src[i]);
        } else if (fb_bpp == 24) {
            for (int i = 0; i < n; i++) {
                uint32_t p = pack(src[i]);
                dst[i * 3] = (uint8_t)p;
                dst[i * 3 + 1] = (uint8_t)(p >> 8);
                dst[i * 3 + 2] = (uint8_t)(p >> 16);
            }
        } else {
            uint16_t *d = (uint16_t *)dst;
            for (int i = 0; i < n; i++)
                d[i] = (uint16_t)pack(src[i]);
        }
    }
}

void gfx_flush_all(void)
{
    gfx_flush(0, 0, screen.w, screen.h);
}

uint32_t gfx_display_pixel(int x, int y)
{
    if (!ready || x < 0 || y < 0 || x >= screen.w || y >= screen.h)
        return 0;
    const uint8_t *p = fb + (size_t)y * fb_pitch + (size_t)x * (fb_bpp / 8);
    uint32_t v = fb_bpp == 32 ? *(const uint32_t *)p :
                 fb_bpp == 24 ? (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16)) :
                                *(const uint16_t *)p;
    uint32_t r = (v >> rpos) & ((1u << rsize) - 1);
    uint32_t g = (v >> gpos) & ((1u << gsize) - 1);
    uint32_t b = (v >> bpos) & ((1u << bsize) - 1);

    r = r << (8 - rsize);
    g = g << (8 - gsize);
    b = b << (8 - bsize);
    return GFX_RGB(r, g, b);
}
