/* display.h - what MyOS really knows about the screen, and frame statistics.
 *
 * Physical refresh rate, target FPS, measured FPS and vsync are four different things:
 *   refresh_hz   the monitor's rate. Only reported when it was MEASURED (VGA vertical retrace pulses);
 *                0 = unknown. GRUB/Multiboot do not tell us, and EDID/VBE-DDC need real-mode BIOS calls
 *                that a 32-bit protected-mode kernel cannot make.
 *   target FPS   what the desktop tries to render (Settings > Display).
 *   measured FPS what the desktop really rendered (gfx_stats).
 *   vsync        true only if the retrace status bit was seen toggling at a plausible rate.
 */
#ifndef DISPLAY_H
#define DISPLAY_H
#include "types.h"

struct display_info {
    int  width, height, bpp, pitch;
    bool bochs_vbe;             /* QEMU/Bochs/VirtualBox VBE registers present */
    int  refresh_hz;            /* measured physical refresh, 0 = unknown */
    bool vsync;                 /* retrace status usable for synchronisation */
    bool edid;                  /* EDID read (never true in this release) */
    const char *refresh_src;    /* where the number came from */
};

struct gfx_stats {
    uint32_t frames;            /* frames rendered since start */
    uint32_t fps;               /* frames in the last full second */
    uint32_t fps_avg;           /* average over the last ~10 seconds */
    uint32_t render_us;         /* last frame: drawing the scene (windows, dock, menus) */
    uint32_t present_us;        /* last frame: copying changed pixels to the frame buffer */
    uint32_t frame_us;          /* last frame: start to start */
    uint32_t dirty_rows;        /* last frame: rows of the frame buffer that changed */
    uint32_t pixels;            /* last frame: pixels written to the frame buffer */
    uint32_t missed;            /* frames that took longer than 1.5 x the target */
    uint32_t target_fps;
};

extern struct gfx_stats gfx_stats;

void display_probe(void);                           /* call once after the timer runs */
const struct display_info *display_get(void);
bool display_wait_vsync(uint32_t max_us);           /* true if a retrace start was seen */
int  display_effective_fps(void);                   /* target FPS after Auto and capability limits */
void gfx_fb_info(int *bpp, int *pitch);
#endif
