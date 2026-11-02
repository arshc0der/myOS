/*
 * taskmgr.c - "Task Manager": every kernel task with its state and CPU use,
 * and a button to end a task. A second example of an app with menus,
 * a popup menu and a confirm dialog (see guiapi.h).
 */
#include "guiapi.h"
#include "input.h"
#include "sched.h"
#include "timer.h"
#include "kerror.h"
#include "kprintf.h"
#include "kstring.h"

#define MAXT 48
#define ROW_H 22
#define HEAD_H 54
#define FOOT_H 44

struct row { pid_t pid; char name[TASK_NAME_MAX]; int state; int prio; uint32_t switches; int cpu; };

struct tm_state {
    struct row rows[MAXT];
    int n, sel, top;
    uint32_t prev_ticks[MAXT];          /* cpu_ticks at the last sample, per slot */
    pid_t prev_pid[MAXT];
    uint32_t last_sample, last_total;
    pid_t victim;
};

enum { C_END = 1, C_REFRESH, C_CLOSE };
enum { D_END = 1 };

static const struct gui_menu_item task_items[] = {
    { "End Task", C_END }, { "Refresh", C_REFRESH }, { "-", 0 }, { "Close Window", C_CLOSE },
};
static const struct gui_menu menus[] = { { "Task", task_items, (int)ARRAY_SIZE(task_items) } };

struct collect { struct tm_state *st; struct row tmp[MAXT]; uint32_t ticks[MAXT]; int n; };

static void collect_cb(struct task *t, void *arg)
{
    struct collect *c = arg;

    if (c->n >= MAXT || t->state == TASK_UNUSED)
        return;
    struct row *r = &c->tmp[c->n];
    r->pid = t->pid;
    strlcpy(r->name, t->name, sizeof r->name);
    r->state = (int)t->state;
    r->prio = t->priority;
    r->switches = t->switches;
    r->cpu = 0;
    c->ticks[c->n] = t->cpu_ticks;
    c->n++;
}

static void sample(struct tm_state *st)
{
    static struct collect c;
    uint32_t now = timer_ticks();
    uint32_t dt = now - st->last_sample;

    c.st = st;
    c.n = 0;
    sched_for_each(collect_cb, &c);
    for (int i = 0; i < c.n; i++) {
        int prev = -1;
        for (int j = 0; j < st->n; j++)
            if (st->prev_pid[j] == c.tmp[i].pid)
                prev = j;
        if (prev >= 0 && dt > 0 && c.ticks[i] >= st->prev_ticks[prev])
            c.tmp[i].cpu = (int)MIN(100u, (c.ticks[i] - st->prev_ticks[prev]) * 100 / dt);
    }
    for (int i = 0; i < c.n; i++) {
        st->prev_pid[i] = c.tmp[i].pid;
        st->prev_ticks[i] = c.ticks[i];
        st->rows[i] = c.tmp[i];
    }
    st->n = c.n;
    st->last_sample = now;
    if (st->sel >= st->n)
        st->sel = st->n - 1;
}

static int visible(const struct gui_window *w) { return MAX(1, (w->ch - HEAD_H - FOOT_H) / ROW_H); }

static void tm_open(struct gui_window *w)
{
    static struct tm_state st;

    memset(&st, 0, sizeof st);
    st.sel = -1;
    st.last_sample = timer_ticks();
    w->priv = &st;
    sample(&st);
    sample(&st);
}

static const char *state_name(int s)
{
    switch (s) {
    case TASK_RUNNING:  return "running";
    case TASK_READY:    return "ready";
    case TASK_SLEEPING: return "sleeping";
    case TASK_BLOCKED:  return "blocked";
    default:            return "finished";
    }
}

static void tm_draw(struct gui_window *w)
{
    struct tm_state *st = w->priv;
    struct gfx_surface *s = w->surf;
    const struct gui_theme *t = gui_th;
    char line[48];

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, t->win);
    ksnprintf(line, sizeof line, "%d tasks", st->n);
    gfx_text(s, 20, 14, "Task Manager", t->text, GFX_TRANSPARENT, 2);
    gfx_text(s, w->cw - 20 - gfx_text_width(line, 1), 22, line, t->dim, GFX_TRANSPARENT, 1);
    gfx_fill_rect(s, 0, 38, w->cw, 16, t->side);
    gfx_text(s, 20, 42, "PID", t->dim, GFX_TRANSPARENT, 1);
    gfx_text(s, 66, 42, "Name", t->dim, GFX_TRANSPARENT, 1);
    gfx_text(s, 230, 42, "State", t->dim, GFX_TRANSPARENT, 1);
    gfx_text(s, 330, 42, "CPU", t->dim, GFX_TRANSPARENT, 1);
    gfx_text(s, 390, 42, "Switches", t->dim, GFX_TRANSPARENT, 1);
    int vis = visible(w);
    for (int i = 0; i < vis && st->top + i < st->n; i++) {
        const struct row *r = &st->rows[st->top + i];
        int y = HEAD_H + i * ROW_H;
        bool sel = st->top + i == st->sel;
        if (sel)
            gfx_fill_round_rect(s, 8, y, w->cw - 24, ROW_H - 2, 6, t->sel);
        uint32_t fg = sel ? t->sel_text : t->text;
        ksnprintf(line, sizeof line, "%d", r->pid);
        gfx_text(s, 20, y + 7, line, fg, GFX_TRANSPARENT, 1);
        gfx_text(s, 66, y + 7, r->name, fg, GFX_TRANSPARENT, 1);
        gfx_text(s, 230, y + 7, state_name(r->state), fg, GFX_TRANSPARENT, 1);
        ksnprintf(line, sizeof line, "%d%%", r->cpu);
        gfx_text(s, 330, y + 7, line, fg, GFX_TRANSPARENT, 1);
        ksnprintf(line, sizeof line, "%u", r->switches);
        gfx_text(s, 390, y + 7, line, fg, GFX_TRANSPARENT, 1);
    }
    gui_scrollbar(s, w->cw - 8, HEAD_H, vis * ROW_H, st->n, vis, st->top);
    gfx_hline(s, 0, w->ch - FOOT_H, w->cw, t->line);
    gui_button(s, w->cw - 120, w->ch - FOOT_H + 9, 100, 26, "End Task", st->sel >= 0);
    gfx_text(s, 20, w->ch - FOOT_H + 18, "Click a task, then End Task (or press Delete).", t->dim, GFX_TRANSPARENT, 1);
}

static void ask_end(struct gui_window *w, struct tm_state *st)
{
    char msg[96];

    if (st->sel < 0 || st->sel >= st->n)
        return;
    const struct row *r = &st->rows[st->sel];
    st->victim = r->pid;
    if (r->pid <= 1 || r->pid == task_current()->pid) {
        gui_alert(w, "Task Manager", "This task keeps the system running and cannot be ended.");
        return;
    }
    ksnprintf(msg, sizeof msg, "End \"%s\" (PID %d)?", r->name, r->pid);
    gui_confirm(w, "End Task", msg, D_END);
}

static void tm_dialog(struct gui_window *w, int id, bool ok, const char *text)
{
    struct tm_state *st = w->priv;
    (void)text;

    if (id == D_END && ok) {
        int rc = task_kill(st->victim);
        if (rc < 0)
            gui_alert(w, "Task Manager", "Could not end that task.");
        sample(st);
    }
    gui_redraw();
}

static void tm_command(struct gui_window *w, int id)
{
    struct tm_state *st = w->priv;

    if (id == C_END) ask_end(w, st);
    else if (id == C_REFRESH) sample(st);
    else if (id == C_CLOSE) gui_close(w);
    gui_redraw();
}

static void tm_click(struct gui_window *w, int x, int y, int buttons)
{
    struct tm_state *st = w->priv;

    if (y >= w->ch - FOOT_H) {
        if (gui_hit(x, y, w->cw - 120, w->ch - FOOT_H + 9, 100, 26))
            ask_end(w, st);
    } else if (y >= HEAD_H) {
        int i = st->top + (y - HEAD_H) / ROW_H;
        if (i < st->n) {
            st->sel = i;
            if (buttons & GUI_BTN_RIGHT) {
                static const struct gui_menu_item it[] = { { "End Task", C_END }, { "Refresh", C_REFRESH } };
                gui_popup(w, x, y, it, 2);
            }
        }
    }
    gui_redraw();
}

static void tm_key(struct gui_window *w, int key, int mods)
{
    struct tm_state *st = w->priv;
    int vis = visible(w);

    (void)mods;
    if (key == KEY_UP && st->sel > 0) st->sel--;
    else if (key == KEY_DOWN && st->sel < st->n - 1) st->sel++;
    else if (key == KEY_DELETE) ask_end(w, st);
    else if (key == KEY_F(5)) sample(st);
    if (st->sel >= 0) {
        if (st->sel < st->top) st->top = st->sel;
        if (st->sel >= st->top + vis) st->top = st->sel - vis + 1;
    }
    gui_redraw();
}

static void tm_tick(struct gui_window *w)
{
    struct tm_state *st = w->priv;

    if (timer_ticks() - st->last_sample >= 100) {         /* once a second */
        sample(st);
        gui_redraw();
    }
}

static void tm_icon(struct gfx_surface *s, int x, int y, int sz, uint32_t g, uint32_t tile)
{
#define P(f) ((sz * (f)) / 100)
    gfx_fill_round_rect(s, x + P(16), y + P(22), P(68), P(48), P(6), g);
    gfx_fill_round_rect(s, x + P(21), y + P(27), P(58), P(38), P(3), tile);
    int h[5] = { 12, 24, 16, 30, 20 };
    for (int i = 0; i < 5; i++)
        gfx_fill_rect(s, x + P(26) + i * P(10), y + P(60) - P(h[i]), P(6), P(h[i]), g);
    gfx_fill_rect(s, x + P(36), y + P(76), P(28), MAX(P(5), 1), g);
#undef P
}

static const struct gui_app taskmgr_app = {
    .name = "Task Manager", .title = "Task Manager", .cw = 520, .ch = 400, .single = true, .dock = 45,
    .open = tm_open, .draw = tm_draw, .click = tm_click, .key = tm_key, .tick = tm_tick, .icon = tm_icon,
    .menus = menus, .nmenus = (int)ARRAY_SIZE(menus), .command = tm_command, .dialog = tm_dialog,
};
GUI_APP(taskmgr_app);
