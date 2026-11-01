/* activity.c - "Activity" (system information and task list). */
#include "guiapi.h"
#include "hw.h"
#include "pmm.h"
#include "heap.h"
#include "sched.h"
#include "timer.h"
#include "multiboot.h"
#include "version.h"
#include "kprintf.h"
#include "kstring.h"

struct rowctx { struct gfx_surface *s; int y, max; };

static void bar(struct gfx_surface *s, int x, int y, int w, int pct)
{
    gfx_fill_round_rect(s, x, y, w, 8, 4, gui_th->line);
    if (pct > 0)
        gfx_fill_round_rect(s, x, y, MAX(w * pct / 100, 8), 8, 4, gui_th->accent);
}

static void task_row(struct task *t, void *arg)
{
    struct rowctx *c = arg;
    char line[64];
    const char *st = t->state == TASK_RUNNING ? "running" : t->state == TASK_READY ? "ready" :
                     t->state == TASK_SLEEPING ? "sleeping" : t->state == TASK_BLOCKED ? "blocked" : "zombie";

    if (c->y > c->max)
        return;
    ksnprintf(line, sizeof line, "%3d  %-12s %s", t->pid, t->name, st);
    gfx_text(c->s, 20, c->y, line, gui_th->text, GFX_TRANSPARENT, 1);
    c->y += 13;
}

static void activity_draw(struct gui_window *w)
{
    struct gfx_surface *s = w->surf;
    char line[100], vendor[13], brand[49];
    int y = 14;

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, gui_th->win);
    gfx_text(s, 20, y, "MyOS " MYOS_VERSION, gui_th->text, GFX_TRANSPARENT, 2);
    y += 28;
    cpu_vendor(vendor);
    if (cpu_brand(brand)) {
        const char *b = brand;
        while (*b == ' ') b++;
        ksnprintf(line, sizeof line, "Processor   %s", b);
    } else {
        ksnprintf(line, sizeof line, "Processor   %s", vendor);
    }
    gfx_text(s, 20, y, line, gui_th->text, GFX_TRANSPARENT, 1);
    y += 15;
    ksnprintf(line, sizeof line, "Display     %dx%d", gui_screen_w(), gui_screen_h());
    gfx_text(s, 20, y, line, gui_th->text, GFX_TRANSPARENT, 1);
    y += 15;
    uint32_t sec = timer_uptime_sec();
    ksnprintf(line, sizeof line, "Uptime      %u:%02u:%02u   Scheduler %s", sec / 3600, (sec / 60) % 60, sec % 60,
              sched_get_policy()->name);
    gfx_text(s, 20, y, line, gui_th->text, GFX_TRANSPARENT, 1);
    y += 26;

    uint32_t total = pmm_total_pages(), used = pmm_used_pages();
    ksnprintf(line, sizeof line, "Memory      %u of %u MiB used", used / 256, total / 256);
    gfx_text(s, 20, y, line, gui_th->text, GFX_TRANSPARENT, 1);
    bar(s, 20, y + 14, w->cw - 40, total ? (int)(used * 100 / total) : 0);
    y += 36;

    struct heap_stats h;
    heap_get_stats(&h);
    ksnprintf(line, sizeof line, "Kernel heap %u of %u KiB, %u allocations", h.used_bytes / 1024,
              h.total_bytes / 1024, h.alloc_count);
    gfx_text(s, 20, y, line, gui_th->text, GFX_TRANSPARENT, 1);
    bar(s, 20, y + 14, w->cw - 40, h.total_bytes ? (int)(h.used_bytes * 100ull / h.total_bytes) : 0);
    y += 40;

    gfx_text(s, 20, y, "Tasks", gui_th->dim, GFX_TRANSPARENT, 1);
    gfx_hline(s, 20, y + 12, w->cw - 40, gui_th->line);
    struct rowctx ctx = { s, y + 18, w->ch - 14 };
    sched_for_each(task_row, &ctx);
}

static void activity_icon(struct gfx_surface *s, int x, int y, int sz, uint32_t g, uint32_t tile)
{
#define P(f) ((sz * (f)) / 100)
    (void)tile;
    static const int hgt[5] = { 26, 44, 34, 58, 40 };
    for (int i = 0; i < 5; i++)
        gfx_fill_round_rect(s, x + P(18) + i * P(13), y + P(78) - P(hgt[i]), P(9), P(hgt[i]), 2, g);
#undef P
}

static const struct gui_app activity_app = {
    .name = "Activity", .title = "Activity", .cw = 470, .ch = 340, .single = true, .dock = 40,
    .draw = activity_draw, .icon = activity_icon,
};
GUI_APP(activity_app);
