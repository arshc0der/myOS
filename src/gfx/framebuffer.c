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
#include "display.h"
#include "timer.h"
#include "multiboot.h"
#include "pmm.h"
#include "vmm.h"
#include "kstring.h"
#include "log.h"
#include "hw.h"
#include "io.h"

static struct gfx_surface screen;
static bool ready;
static uint8_t *fb;                     /* real framebuffer */
static uint32_t fb_pitch, fb_bpp;
static uint8_t rpos, rsize, gpos, gsize, bpos, bsize;
static bool fast32;                     /* 32 bpp, standard XRGB: plain copy */

static paddr_t back_addr;
/* What the display currently shows. gfx_flush only writes the pixels that differ: the frame buffer is
 * uncached memory, so every byte not written is a big saving (a still desktop costs almost nothing). */
static uint32_t *shadow;
static paddr_t shadow_addr;
static uint32_t shadow_pages;
static const uint32_t *shadow_for;
static bool shadow_ok;
static uint32_t dim_mul = 256;                 /* brightness: 256 = full, lower darkens every pixel written */

static inline uint32_t dimpx(uint32_t p)
{
    return dim_mul >= 256 ? p : ((((p & 0xFF00FFu) * dim_mul) >> 8) & 0xFF00FFu) | ((((p & 0x00FF00u) * dim_mul) >> 8) & 0x00FF00u);
}

void gfx_set_brightness(int pct)
{
    pct = pct < 10 ? 10 : pct > 100 ? 100 : pct;
    uint32_t m = (uint32_t)pct * 256 / 100;
    if (m != dim_mul) {
        dim_mul = m;
        shadow_ok = false;                      /* everything must be sent again */
    }
}

static uint32_t back_pages;

bool gfx_available(void) { return ready; }
struct gfx_surface *gfx_screen(void) { return &screen; }
int gfx_width(void) { return screen.w; }
int gfx_height(void) { return screen.h; }

/* mode 1: red in the high byte (XRGB, the usual layout); mode 2: blue in the high byte (XBGR) */
static void set_format(int mode)
{
    if (fb_bpp == 16) {
        rsize = 5; gsize = 6; bsize = 5;
        if (mode == 2) { bpos = 11; gpos = 5; rpos = 0; } else { rpos = 11; gpos = 5; bpos = 0; }
    } else {
        rsize = gsize = bsize = 8;
        gpos = 8;
        if (mode == 2) { bpos = 16; rpos = 0; } else { rpos = 16; bpos = 0; }
    }
}

static void update_fast(void)
{
    fast32 = fb_bpp == 32 && rpos == 16 && gpos == 8 && bpos == 0 &&
             rsize == 8 && gsize == 8 && bsize == 8;
}

/* Settings > Display: 0 = what the boot loader said, 1 = RGB, 2 = BGR. Fixes wrong colours. */
static int fmt_choice;                  /* last Settings choice: 0 auto, 1 RGB, 2 BGR */

void gfx_set_pixel_format(int mode)
{
    if (!ready)
        return;
    fmt_choice = mode;
    shadow_ok = false;
    const struct boot_fb *f = &boot_info.fb;
    if (mode == 0) {
        rpos = f->red_pos; rsize = f->red_size; gpos = f->green_pos; gsize = f->green_size;
        bpos = f->blue_pos; bsize = f->blue_size;
        if (!(rsize && gsize && bsize))
            set_format(1);
    } else {
        set_format(mode);
    }
    update_fast();
    gfx_flush_all();
}

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

    back_addr = back;
    back_pages = ((f->width * f->height * 4) + PAGE_SIZE - 1) / PAGE_SIZE;
    fb = (uint8_t *)(uint32_t)f->addr;
    fb_pitch = f->pitch;
    fb_bpp = f->bpp;
    rpos = f->red_pos;   rsize = f->red_size;
    gpos = f->green_pos; gsize = f->green_size;
    bpos = f->blue_pos;  bsize = f->blue_size;
    /* The boot loader's colour description is sometimes missing or wrong (a missing
     * blue channel turns white into yellow). Only trust it when it is plausible. */
    bool sane = rsize >= 1 && rsize <= 8 && gsize >= 1 && gsize <= 8 && bsize >= 1 && bsize <= 8 &&
                rpos + rsize <= fb_bpp && gpos + gsize <= fb_bpp && bpos + bsize <= fb_bpp &&
                rpos != gpos && gpos != bpos && rpos != bpos;
    if (!sane)
        set_format(1);
    if (boot_has_arg("fbfmt=bgr"))
        set_format(2);
    else if (boot_has_arg("fbfmt=rgb"))
        set_format(1);
    update_fast();

    memset((void *)back, 0, (size_t)f->width * f->height * 4);
    gfx_surface_init(&screen, (uint32_t *)back, (int)f->width, (int)f->height);
    ready = true;
    log_info("gfx", "framebuffer %ux%u, %u bpp at 0x%08x (%s; r%u/%u g%u/%u b%u/%u)", f->width, f->height,
             f->bpp, (uint32_t)f->addr, fast32 ? "direct copy" : "converted", rpos, rsize, gpos, gsize, bpos, bsize);
    return true;
}

static inline uint32_t pack(uint32_t c)
{
    uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;

    return ((r >> (8 - rsize)) << rpos) | ((g >> (8 - gsize)) << gpos) |
           ((b >> (8 - bsize)) << bpos);
}

static void shadow_prepare(void)
{
    uint32_t pages = ((uint32_t)screen.w * (uint32_t)screen.h * 4 + PAGE_SIZE - 1) / PAGE_SIZE;

    if (shadow && shadow_for == screen.px && shadow_pages == pages)
        return;
    if (shadow)
        pmm_free_pages(shadow_addr, shadow_pages);
    shadow_addr = pmm_alloc_contiguous(pages);
    shadow = shadow_addr ? (uint32_t *)shadow_addr : NULL;
    shadow_pages = pages;
    shadow_for = screen.px;
    shadow_ok = false;
}

void gfx_fb_info(int *bpp, int *pitch)
{
    *bpp = fb_bpp;
    *pitch = (int)fb_pitch;
}

void gfx_flush(int x, int y, int w, int h)
{
    if (!ready)
        return;
    int x0 = MAX(x, 0), y0 = MAX(y, 0);
    int x1 = MIN(x + w, screen.w), y1 = MIN(y + h, screen.h);

    if (x0 >= x1 || y0 >= y1)
        return;
    shadow_prepare();
    for (int j = y0; j < y1; j++) {
        const uint32_t *src = &screen.px[j * screen.stride + x0];
        uint32_t *sh = shadow ? &shadow[(size_t)j * (size_t)screen.w + (size_t)x0] : NULL;
        int n = x1 - x0, a = 0, b = n;

        if (sh && shadow_ok) {                      /* narrow the row to the changed span */
            while (a < n && src[a] == sh[a]) a++;
            if (a == n)
                continue;
            while (b > a && src[b - 1] == sh[b - 1]) b--;
        }
        if (sh)
            memcpy(sh + a, src + a, (size_t)(b - a) * 4);
        gfx_stats.dirty_rows++;
        gfx_stats.pixels += (uint32_t)(b - a);
        uint8_t *dst = fb + (size_t)j * fb_pitch + (size_t)(x0 + a) * (fb_bpp / 8);
        src += a;
        n = b - a;

        if (fast32 && dim_mul >= 256) {
            memcpy(dst, src, (size_t)n * 4);
        } else if (fast32 || fb_bpp == 32) {
            uint32_t *d = (uint32_t *)dst;
            for (int i = 0; i < n; i++)
                d[i] = pack(dimpx(src[i]));
        } else if (fb_bpp == 24) {
            for (int i = 0; i < n; i++) {
                uint32_t p = pack(dimpx(src[i]));
                dst[i * 3] = (uint8_t)p;
                dst[i * 3 + 1] = (uint8_t)(p >> 8);
                dst[i * 3 + 2] = (uint8_t)(p >> 16);
            }
        } else {
            uint16_t *d = (uint16_t *)dst;
            for (int i = 0; i < n; i++)
                d[i] = (uint16_t)pack(dimpx(src[i]));
        }
    }
    if (x0 == 0 && y0 == 0 && x1 == screen.w && y1 == screen.h)
        shadow_ok = shadow != NULL;                 /* a full pass makes the shadow trustworthy */
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

/* ------------------------------------------------------------------ */
/* Changing the resolution at run time                                 */
/*                                                                     */
/* Virtual machines (QEMU / UTM "standard VGA", Bochs, VirtualBox) have a */
/* "Bochs VBE" display: two I/O ports let us pick a new size without the */
/* BIOS. Real graphics cards cannot be switched like this without a   */
/* driver for that card, so there gfx_mode_count() is 0.              */
/* ------------------------------------------------------------------ */

#define DISPI_INDEX 0x1CE
#define DISPI_DATA  0x1CF
#define DI_ID 0
#define DI_XRES 1
#define DI_YRES 2
#define DI_BPP 3
#define DI_ENABLE 4
#define DI_VIRT_W 6
#define DI_VIRT_H 7
#define DI_XOFF 8
#define DI_YOFF 9
#define DI_VRAM64K 0xA

static uint16_t di_r(int i) { outw(DISPI_INDEX, (uint16_t)i); return inw(DISPI_DATA); }
static void di_w(int i, uint16_t v) { outw(DISPI_INDEX, (uint16_t)i); outw(DISPI_DATA, v); }

static const int mode_table[][2] = {
    { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1366, 768 },
    { 1440, 900 }, { 1600, 900 }, { 1920, 1080 },
};

static int dispi_state = -1;            /* -1 unknown, 0 absent, 1 present */
static uint32_t dispi_vram;

static bool dispi_present(void)
{
    if (dispi_state >= 0)
        return dispi_state == 1;
    dispi_state = 0;
    if (!ready)
        return false;
    struct pci_device list[64];
    int n = pci_scan(list, 64);
    bool dev = false;

    for (int i = 0; i < n && i < 64; i++)
        if ((list[i].vendor == 0x1234 && list[i].device == 0x1111) ||
            (list[i].vendor == 0x80EE && list[i].device == 0xBEEF))
            dev = true;
    uint16_t id = di_r(DI_ID);
    log_info("gfx", "display adapter: pci=%d dispi id=%x", dev, id);
    if (id < 0xB0C0 || id > 0xB0CF)
        return false;
    (void)dev;
    dispi_vram = (uint32_t)di_r(DI_VRAM64K) * 65536u;
    if (dispi_vram < 1024u * 768 * 4)
        dispi_vram = 16u * 1024 * 1024;     /* register not implemented: QEMU default is 16 MiB */
    dispi_state = 1;
    return true;
}

int gfx_mode_count(void)
{
    if (!dispi_present() || boot_has_arg("nomodeset"))
        return 0;
    int n = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(mode_table); i++)
        if ((uint32_t)mode_table[i][0] * (uint32_t)mode_table[i][1] * 4 <= dispi_vram)
            n++;
    return n;
}

void gfx_mode_at(int index, int *w, int *h)
{
    int n = 0;

    for (unsigned i = 0; i < ARRAY_SIZE(mode_table); i++)
        if ((uint32_t)mode_table[i][0] * (uint32_t)mode_table[i][1] * 4 <= dispi_vram) {
            if (n++ == index) {
                *w = mode_table[i][0];
                *h = mode_table[i][1];
                return;
            }
        }
    *w = *h = 0;
}

static void di_program(int w, int h)
{
    di_w(DI_ENABLE, 0);
    di_w(DI_XRES, (uint16_t)w);
    di_w(DI_YRES, (uint16_t)h);
    di_w(DI_BPP, 32);
    di_w(DI_VIRT_W, (uint16_t)w);
    di_w(DI_XOFF, 0);
    di_w(DI_YOFF, 0);
    di_w(DI_ENABLE, 0x01 | 0x40);               /* enabled + linear framebuffer */
}

bool gfx_set_mode(int w, int h)
{
    if (!ready || gfx_mode_count() == 0) {
        log_warn("gfx", "mode change: no switchable display");
        return false;
    }
    if ((uint32_t)w * (uint32_t)h * 4 > dispi_vram) {
        log_warn("gfx", "mode change: %dx%d does not fit in video memory", w, h);
        return false;
    }
    if (w == screen.w && h == screen.h)
        return true;
    uint32_t pages = ((uint32_t)w * (uint32_t)h * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    paddr_t nb = pmm_alloc_contiguous(pages);
    if (!nb) {
        log_warn("gfx", "mode change: out of memory");
        return false;
    }
    int ow = screen.w, oh = screen.h;

    di_program(w, h);
    if (di_r(DI_XRES) != w || di_r(DI_YRES) != h) {             /* refused: go back */
        log_warn("gfx", "mode change: adapter refused %dx%d (reads back %dx%d)", w, h, di_r(DI_XRES), di_r(DI_YRES));
        di_program(ow, oh);
        pmm_free_pages(nb, pages);
        return false;
    }
    if (vmm_enabled())
        vmm_map_range((vaddr_t)(uint32_t)boot_info.fb.addr, (paddr_t)boot_info.fb.addr,
                      ((uint32_t)w * (uint32_t)h * 4 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1),
                      VMM_WRITE | VMM_NOCACHE);
    memset((void *)nb, 0, (size_t)pages * PAGE_SIZE);
    paddr_t old = back_addr;
    uint32_t oldpages = back_pages;
    gfx_surface_init(&screen, (uint32_t *)nb, w, h);
    back_addr = nb;
    back_pages = pages;
    pmm_free_pages(old, oldpages);
    fb_pitch = (uint32_t)w * 4;
    fb_bpp = 32;
    set_format(fmt_choice == 2 ? 2 : 1);   /* the new 32 bit mode is plain XRGB unless the user chose BGR */
    update_fast();
    boot_info.fb.width = (uint32_t)w;
    boot_info.fb.height = (uint32_t)h;
    boot_info.fb.pitch = fb_pitch;
    boot_info.fb.bpp = 32;
    log_info("gfx", "resolution changed to %dx%d", w, h);
    return true;
}
