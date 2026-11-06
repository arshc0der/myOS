/*
 * display.c - display capability detection and the frame statistics.
 *
 * What is detected (and how):
 *  - resolution, depth, pitch: from the frame buffer GRUB gave us (gfx_fb_info).
 *  - Bochs/QEMU VBE registers: index/data ports 0x1CE/0x1CF (ID 0xB0C0..0xB0C5).
 *  - refresh rate: by counting vertical-retrace pulses in the VGA input status register (0x3DA bit 3)
 *    for 300 ms against the CPU time-stamp counter. This works on BIOS boots of VGA-compatible adapters
 *    and in QEMU's standard VGA. On UEFI/GOP and many modern GPUs the register does not toggle, and then
 *    the refresh rate is reported as unknown. Nothing is guessed.
 */
#include "display.h"
#include "gfx.h"
#include "io.h"
#include "timer.h"
#include "sys.h"
#include "kprintf.h"
#include "log.h"

struct gfx_stats gfx_stats;
static struct display_info di = { .refresh_src = "not measured" };

static int measure_retrace(void)
{
    uint32_t t0 = timer_us(), now;
    int edges = 0, last = inb(0x3DA) & 8;
    uint32_t span = 300000;

    do {
        int v = inb(0x3DA) & 8;
        if (v && !last)
            edges++;
        last = v;
        now = timer_us();
    } while ((uint32_t)(now - t0) < span);
    if (edges < 5)
        return 0;
    int hz = (int)(((uint64_t)edges * 1000000u + (now - t0) / 2) / (now - t0));
    return hz >= 24 && hz <= 244 ? hz : 0;
}

void display_probe(void)
{
    uint16_t id;

    timer_hires_init();
    gfx_fb_info(&di.bpp, &di.pitch);
    di.width = gfx_width();
    di.height = gfx_height();
    outw(0x1CE, 0);
    id = inw(0x1CF);
    di.bochs_vbe = id >= 0xB0C0 && id <= 0xB0CF;
    di.edid = false;
    di.refresh_hz = 0;
    di.vsync = false;
    di.refresh_src = "unknown (no retrace signal)";
    if (timer_hires_ok()) {
        int hz = measure_retrace();
        if (hz) {
            di.refresh_hz = hz;
            di.vsync = true;
            di.refresh_src = "measured (VGA retrace)";
        }
    }
    log_info("display", "%dx%d %d bpp pitch %d, refresh %s%d Hz", di.width, di.height, di.bpp, di.pitch,
             di.refresh_hz ? "" : "unknown ", di.refresh_hz);
}

const struct display_info *display_get(void) { return &di; }

bool display_wait_vsync(uint32_t max_us)
{
    if (!di.vsync)
        return false;
    uint32_t t0 = timer_us();
    while (inb(0x3DA) & 8)                          /* let a retrace in progress finish */
        if ((uint32_t)(timer_us() - t0) > max_us)
            return false;
    while (!(inb(0x3DA) & 8))                       /* wait for the next one to start */
        if ((uint32_t)(timer_us() - t0) > max_us)
            return false;
    return true;
}

int display_effective_fps(void)
{
    int want = sys_prefs.refresh_hz;                /* 0 = Auto */

    if (want <= 0)
        want = di.refresh_hz ? di.refresh_hz : 60;
    if (di.refresh_hz && want > di.refresh_hz)
        want = di.refresh_hz;                       /* more frames than the monitor shows are wasted */
    if (want < 20) want = 20;
    if (want > 240) want = 240;
    return want;
}
