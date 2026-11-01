/*
 * terminal.c - the Terminal app: runs the normal shell inside a window.
 * While the window exists the console output goes to its text grid, and
 * keys typed in the window are handed to the shell task.
 */
#include "guiapi.h"
#include "tgrid.h"
#include "console.h"
#include "input.h"
#include "shell.h"
#include "heap.h"
#include "sched.h"
#include "sync.h"
#include "io.h"
#include "kstring.h"
#include "serial.h"
#include "multiboot.h"

struct term {
    struct tgrid tg;
    pid_t shell_pid;
    struct gui_window *win;
};

static struct term *cur;                 /* the one Terminal (single = true) */

#define KQ 64
static int keyq[KQ];
static volatile uint32_t kq_head, kq_tail;
static struct wait_queue kq_wait = WAIT_QUEUE_INIT(kq_wait);

static void kq_push(int key)
{
    uint32_t f = irq_save();
    uint32_t next = (kq_head + 1) % KQ;

    if (next != kq_tail) {
        keyq[kq_head] = key;
        kq_head = next;
        wq_wake_all(&kq_wait);
    }
    irq_restore(f);
}

static int term_getkey(void)
{
    int k;
    uint32_t f = irq_save();

    while (kq_head == kq_tail)
        wq_wait_locked(&kq_wait);
    k = keyq[kq_tail];
    kq_tail = (kq_tail + 1) % KQ;
    irq_restore(f);
    return k;
}

/* ---- console back end: shell output goes to the grid ---- */

static void gt_putc(char c)
{
    if (!cur)
        return;
    preempt_disable();
    tg_putc(&cur->tg, c);
    preempt_enable();
}
static void gt_clear(void) { if (cur) tg_clear(&cur->tg); gui_redraw(); }
static void gt_set_color(uint8_t a) { if (cur) cur->tg.attr = a; }
static void gt_scroll(int n) { if (cur) tg_scroll_view(&cur->tg, n); gui_redraw(); }
static void gt_flush(void) { gui_redraw(); }

static const struct console_backend gui_backend = {
    .name = "gui-terminal", .putc = gt_putc, .clear = gt_clear,
    .set_color = gt_set_color, .scroll = gt_scroll, .flush = gt_flush,
};

static void shell_task(void *arg)
{
    (void)arg;
    shell_run();
    kq_push(-2);                         /* the user typed exit: close the window */
}

static void term_size(int *cw, int *ch)
{
    *cw = MIN(gui_screen_w() - 160, 920) / 8 * 8;
    *ch = MIN(gui_screen_h() - 200, 560) / 16 * 16;
}

static void term_open(struct gui_window *w)
{
    struct term *t = kzalloc(sizeof *t);

    if (!t || !tg_init(&t->tg, w->cw / 8, w->ch / 16, 300)) {
        kfree(t);
        gui_close(w);
        return;
    }
    t->win = w;
    w->priv = t;
    kq_head = kq_tail = 0;
    preempt_disable();
    cur = t;
    preempt_enable();
    console_set_backend(&gui_backend);
    console_set_key_source(term_getkey);
    t->shell_pid = task_create("shell", shell_task, NULL);
}

static void term_close(struct gui_window *w)
{
    struct term *t = w->priv;

    if (!t)
        return;
    if (boot_has_arg("guitest")) {                   /* let the automatic test read the screen */
        serial_write("GUITEST-TERM-BEGIN\n");
        for (int r = 0; r < t->tg.rows; r++) {
            char line[200];
            int n = 0;
            for (int c = 0; c < t->tg.cols && n < 190; c++) {
                char ch = (char)tg_cell(&t->tg, r, c)->ch;
                line[n++] = ch ? ch : ' ';
            }
            while (n > 0 && line[n - 1] == ' ')
                n--;
            line[n] = '\0';
            serial_write(line);
            serial_write("\n");
        }
        serial_write("GUITEST-TERM-END\n");
    }
    if (t->shell_pid > 0)
        task_kill(t->shell_pid);
    preempt_disable();
    cur = NULL;
    preempt_enable();
    console_set_key_source(NULL);
    tg_free(&t->tg);
    kfree(t);
    w->priv = NULL;
}

static void term_draw(struct gui_window *w)
{
    struct term *t = w->priv;
    int f, l;

    if (!t)
        return;
    preempt_disable();
    tg_render(&t->tg, w->surf, 0, 0, 16, false, &f, &l);
    preempt_enable();
    gfx_fill_rect(w->surf, t->tg.cols * 8, 0, w->cw - t->tg.cols * 8, w->ch,
                  tg_light_mode ? GFX_WHITE : GFX_BLACK);
    gfx_fill_rect(w->surf, 0, t->tg.rows * 16, w->cw, w->ch - t->tg.rows * 16,
                  tg_light_mode ? GFX_WHITE : GFX_BLACK);
}

static void term_key(struct gui_window *w, int key, int mods)
{
    (void)w; (void)mods;
    kq_push(key);
}

static void term_tick(struct gui_window *w)
{
    (void)w;
    while (kq_head != kq_tail && keyq[kq_tail] == -2) {      /* shell ended */
        kq_tail = (kq_tail + 1) % KQ;
        if (cur) {
            cur->shell_pid = 0;
            gui_close(cur->win);
        }
    }
}

static void term_icon(struct gfx_surface *s, int x, int y, int sz, uint32_t g, uint32_t tile)
{
#define P(f) ((sz * (f)) / 100)
    (void)tile;
    for (int t = 0; t < 2; t++) {
        gfx_line(s, x + P(26), y + P(32) + t, x + P(44), y + P(50) + t, g);
        gfx_line(s, x + P(44), y + P(50) + t, x + P(26), y + P(68) + t, g);
    }
    gfx_fill_rect(s, x + P(50), y + P(66), P(24), MAX(P(6), 2), g);
#undef P
}

static const struct gui_app terminal_app = {
    .name = "Terminal", .title = "Terminal", .cw = 656, .ch = 400, .size = term_size,
    .single = true, .dock = 10, .dark_tile = true,
    .open = term_open, .close = term_close, .draw = term_draw, .key = term_key,
    .tick = term_tick, .icon = term_icon,
};
GUI_APP(terminal_app);
