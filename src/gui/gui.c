/*
 * gui.c - desktop, window manager and the built-in applications.
 *
 * Structure:
 *   - a window list in z-order (last = in front)
 *   - each window owns a surface for its client area; apps draw into it
 *   - every frame we draw wallpaper, icons, windows, taskbar, menu, cursor
 *     into the screen back buffer and flush it to the display
 *   - the Terminal window shows a text grid that the shell prints into
 *     (the GUI installs its own console back end while it runs)
 */
#include "gui.h"
#include "io.h"
#include "gfx.h"
#include "tgrid.h"
#include "fbcon.h"
#include "input.h"
#include "console.h"
#include "shell.h"
#include "sched.h"
#include "sync.h"
#include "timer.h"
#include "vfs.h"
#include "pmm.h"
#include "heap.h"
#include "hw.h"
#include "kernel.h"
#include "kprintf.h"
#include "kstring.h"
#include "kerror.h"
#include "multiboot.h"
#include "log.h"
#include "serial.h"

/* ------------------------------------------------------------------ */
/* Colours and sizes                                                   */
/* ------------------------------------------------------------------ */

#define C_BG_TOP     GFX_RGB(58, 72, 168)
#define C_BG_BOTTOM  GFX_RGB(236, 128, 150)
#define C_TASKBAR    GFX_RGB(28, 32, 44)
#define C_TASKBAR_HI GFX_RGB(52, 60, 82)
#define C_ACCENT     GFX_RGB(10, 132, 255)
#define C_TITLE_ON   GFX_RGB(40, 100, 210)
#define C_TITLE_OFF  GFX_RGB(86, 92, 108)
#define C_WIN_BG     GFX_RGB(246, 247, 250)
#define C_TEXT       GFX_RGB(28, 30, 38)
#define C_TEXT_DIM   GFX_RGB(110, 116, 130)
#define C_LIGHT      GFX_RGB(236, 240, 248)

#define TITLE_H    28
#define MENUBAR_H  28
#define TASKBAR_H  84          /* space reserved for the dock */
#define MAX_WINDOWS 8
#define MENU_W     220
#define MENU_ITEM_H 26

enum app_type { APP_TERMINAL, APP_FILES, APP_SYSINFO, APP_NOTEPAD, APP_ABOUT };

#define NOTE_CAP 16384
#define FILES_MAX 96

struct window {
    bool used;
    enum app_type type;
    int x, y;                       /* outer top-left on screen   */
    int cw, ch;                     /* client size                */
    char title[40];
    struct gfx_surface *surf;       /* client area                */

    /* Terminal */
    struct tgrid tg;
    pid_t shell_pid;

    /* Files */
    char path[VFS_PATH_MAX];
    struct dirent ent[FILES_MAX];
    int nent, sel, top;

    /* Notepad */
    char *text;
    int len, cur, top_line;
    char file[VFS_PATH_MAX];
    char status[48];
};

static struct window wins[MAX_WINDOWS];
static int zorder[MAX_WINDOWS];
static int nz;

static struct gfx_surface *wallpaper;
static int sw, sh;
static int mx, my;
static uint8_t mbuttons;
static bool have_mouse;
static bool menu_open;
static bool ptr_mode;            /* F2: arrow keys move the pointer */
static bool running;

static struct window *term_win;
static volatile bool term_dirty;

/* ------------------------------------------------------------------ */
/* Terminal plumbing: console back end + key queue                     */
/* ------------------------------------------------------------------ */

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

static void gt_putc(char c)
{
    if (!term_win)
        return;
    preempt_disable();
    tg_putc(&term_win->tg, c);
    preempt_enable();
}

static void gt_clear(void)
{
    if (term_win)
        tg_clear(&term_win->tg);
    term_dirty = true;
}

static void gt_set_color(uint8_t a)
{
    if (term_win)
        term_win->tg.attr = a;
}

static void gt_scroll(int n)
{
    if (term_win)
        tg_scroll_view(&term_win->tg, n);
    term_dirty = true;
}

static void gt_flush(void) { term_dirty = true; }

static const struct console_backend gui_backend = {
    .name = "gui-terminal",
    .putc = gt_putc,
    .clear = gt_clear,
    .set_color = gt_set_color,
    .scroll = gt_scroll,
    .flush = gt_flush,
};

/* ------------------------------------------------------------------ */
/* Window management                                                   */
/* ------------------------------------------------------------------ */

static int outer_w(const struct window *w) { return w->cw + 2; }
static int outer_h(const struct window *w) { return w->ch + TITLE_H + 1; }

static int win_index(const struct window *w) { return (int)(w - wins); }

static void bring_to_front(int idx)
{
    int pos = -1;

    for (int i = 0; i < nz; i++)
        if (zorder[i] == idx)
            pos = i;
    if (pos < 0)
        return;
    for (int i = pos; i < nz - 1; i++)
        zorder[i] = zorder[i + 1];
    zorder[nz - 1] = idx;
}

static struct window *focused(void)
{
    return nz ? &wins[zorder[nz - 1]] : NULL;
}

static void files_load(struct window *w);
static void notepad_open(const char *path);

static void win_close(struct window *w)
{
    int idx = win_index(w);

    if (w->type == APP_TERMINAL) {
        if (w->shell_pid > 0)
            task_kill(w->shell_pid);
        preempt_disable();
        term_win = NULL;
        preempt_enable();
        tg_free(&w->tg);
    }
    kfree(w->text);
    w->text = NULL;
    gfx_surface_free(w->surf);
    w->surf = NULL;
    w->used = false;
    int j = 0;
    for (int i = 0; i < nz; i++)
        if (zorder[i] != idx)
            zorder[j++] = zorder[i];
    nz = j;
}

static struct window *win_create(enum app_type type, const char *title, int cw, int ch)
{
    int slot = -1;

    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!wins[i].used) {
            slot = i;
            break;
        }
    if (slot < 0)
        return NULL;
    if (cw > sw - 20) cw = sw - 20;
    if (ch > sh - TASKBAR_H - MENUBAR_H - TITLE_H - 20) ch = sh - TASKBAR_H - MENUBAR_H - TITLE_H - 20;

    struct window *w = &wins[slot];
    memset(w, 0, sizeof *w);
    w->surf = gfx_surface_create(cw, ch);
    if (!w->surf)
        return NULL;
    w->used = true;
    w->type = type;
    w->cw = cw;
    w->ch = ch;
    strlcpy(w->title, title, sizeof w->title);
    int off = (nz % 6) * 26;                        /* cascade new windows */
    w->x = MAX(10, (sw - outer_w(w)) / 2 - 60 + off);
    w->y = MAX(MENUBAR_H + 8, (sh - TASKBAR_H - outer_h(w)) / 2 - 30 + off);
    zorder[nz++] = slot;
    return w;
}

/* ------------------------------------------------------------------ */
/* Applications                                                        */
/* ------------------------------------------------------------------ */

/* ---- Terminal ---- */

static void term_shell_task(void *arg)
{
    (void)arg;
    shell_run();
    /* the shell ended (user typed exit): ask the GUI to close the window */
    kq_push(-2);
}

static struct window *open_terminal(void)
{
    if (term_win) {
        bring_to_front(win_index(term_win));
        return term_win;
    }
    int tcw = MIN(sw - 160, 920) / 8 * 8, tch = MIN(sh - TASKBAR_H - MENUBAR_H - 90, 560) / 16 * 16;
    struct window *w = win_create(APP_TERMINAL, "Terminal", tcw, tch);
    if (!w)
        return NULL;
    if (!tg_init(&w->tg, w->cw / 8, w->ch / 16, 300)) {
        win_close(w);
        return NULL;
    }
    preempt_disable();
    term_win = w;
    preempt_enable();
    kq_head = kq_tail = 0;
    w->shell_pid = task_create("shell", term_shell_task, NULL);
    return w;
}

static void draw_terminal(struct window *w)
{
    int f, l;

    preempt_disable();
    tg_render(&w->tg, w->surf, 0, 0, 16, false, &f, &l);
    preempt_enable();
    /* margin pixels beyond the grid */
    gfx_fill_rect(w->surf, w->tg.cols * 8, 0, w->cw - w->tg.cols * 8, w->ch, GFX_BLACK);
    gfx_fill_rect(w->surf, 0, w->tg.rows * 16, w->cw, w->ch - w->tg.rows * 16, GFX_BLACK);
}

/* ---- Files ---- */

static int ent_cmp(const struct dirent *a, const struct dirent *b)
{
    bool da = a->type == VFS_DIR, db = b->type == VFS_DIR;

    if (da != db)
        return da ? -1 : 1;
    return strcmp(a->name, b->name);
}

static void files_load(struct window *w)
{
    struct dirent de;

    w->nent = 0;
    for (uint32_t i = 0; w->nent < FILES_MAX && vfs_readdir(w->path, i, &de) == 0; i++)
        w->ent[w->nent++] = de;
    for (int i = 1; i < w->nent; i++) {                     /* insertion sort */
        struct dirent k = w->ent[i];
        int j = i - 1;
        while (j >= 0 && ent_cmp(&w->ent[j], &k) > 0) {
            w->ent[j + 1] = w->ent[j];
            j--;
        }
        w->ent[j + 1] = k;
    }
    w->sel = 0;
    w->top = 0;
}

static void files_go(struct window *w, const char *path)
{
    char abs[VFS_PATH_MAX];
    struct vfs_stat st;

    if (vfs_normalize(path, abs, sizeof abs) < 0 || vfs_stat(abs, &st) < 0 ||
        st.type != VFS_DIR)
        return;
    strlcpy(w->path, abs, sizeof w->path);
    files_load(w);
}

static struct window *open_files(void)
{
    struct window *w = win_create(APP_FILES, "Files", 520, 340);

    if (!w)
        return NULL;
    strcpy(w->path, "/");
    files_load(w);
    return w;
}

static int files_rows(const struct window *w) { return (w->ch - 40) / 16; }

static void files_activate(struct window *w, int i)
{
    if (i < 0 || i >= w->nent)
        return;
    char full[VFS_PATH_MAX];

    ksnprintf(full, sizeof full, "%s/%s", strcmp(w->path, "/") ? w->path : "", w->ent[i].name);
    if (w->ent[i].type == VFS_DIR)
        files_go(w, full);
    else if (w->ent[i].type == VFS_FILE)
        notepad_open(full);
}

static void files_parent(struct window *w)
{
    char p[VFS_PATH_MAX];

    strlcpy(p, w->path, sizeof p);
    char *slash = strrchr(p, '/');
    if (!slash)
        return;
    if (slash == p)
        p[1] = '\0';
    else
        *slash = '\0';
    files_go(w, p);
}

static void files_key(struct window *w, int key)
{
    int rows = files_rows(w);

    switch (key) {
    case KEY_UP:    if (w->sel > 0) w->sel--; break;
    case KEY_DOWN:  if (w->sel < w->nent - 1) w->sel++; break;
    case KEY_PGUP:  w->sel = MAX(0, w->sel - rows); break;
    case KEY_PGDN:  w->sel = MIN(w->nent - 1, w->sel + rows); break;
    case KEY_HOME:  w->sel = 0; break;
    case KEY_END:   w->sel = MAX(0, w->nent - 1); break;
    case '\n':      files_activate(w, w->sel); break;
    case '\b': case KEY_LEFT: files_parent(w); break;
    case 'r': case KEY_F(5): files_load(w); break;
    case 'd':
        if (w->sel < w->nent) {
            char full[VFS_PATH_MAX];
            ksnprintf(full, sizeof full, "%s/%s", strcmp(w->path, "/") ? w->path : "", w->ent[w->sel].name);
            vfs_unlink(full);
            int s = w->sel;
            files_load(w);
            w->sel = MIN(s, MAX(0, w->nent - 1));
        }
        break;
    }
    if (w->sel < w->top) w->top = w->sel;
    if (w->sel >= w->top + rows) w->top = w->sel - rows + 1;
}

static void draw_files(struct window *w)
{
    struct gfx_surface *s = w->surf;

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, C_WIN_BG);
    gfx_fill_rect(s, 0, 0, w->cw, 22, C_LIGHT);
    gfx_text(s, 8, 7, w->path, C_TEXT, GFX_TRANSPARENT, 1);
    gfx_hline(s, 0, 22, w->cw, GFX_RGB(200, 204, 214));
    int rows = files_rows(w);
    for (int r = 0; r < rows && w->top + r < w->nent; r++) {
        const struct dirent *e = &w->ent[w->top + r];
        int y = 26 + r * 16;
        bool sel = w->top + r == w->sel;
        if (sel)
            gfx_fill_rect(s, 2, y - 2, w->cw - 4, 16, C_ACCENT);
        uint32_t fg = sel ? GFX_WHITE : (e->type == VFS_DIR ? GFX_RGB(20, 80, 190) : C_TEXT);
        gfx_fill_rect(s, 8, y + 1, 10, 8, e->type == VFS_DIR ? GFX_RGB(240, 190, 60) :
                      (e->type == VFS_FILE ? GFX_RGB(150, 170, 200) : GFX_RGB(200, 120, 90)));
        gfx_text(s, 24, y + 1, e->name, fg, GFX_TRANSPARENT, 1);
        if (e->type == VFS_FILE || e->type == VFS_BLOCK) {
            char sz[16];
            ksnprintf(sz, sizeof sz, "%u", e->size);
            gfx_text(s, w->cw - 12 - gfx_text_width(sz, 1), y + 1, sz,
                     sel ? GFX_WHITE : C_TEXT_DIM, GFX_TRANSPARENT, 1);
        }
    }
    if (w->nent == 0)
        gfx_text(s, 12, 40, "(empty folder)", C_TEXT_DIM, GFX_TRANSPARENT, 1);
    gfx_fill_rect(s, 0, w->ch - 18, w->cw, 18, C_LIGHT);
    gfx_text(s, 8, w->ch - 13, "Enter open   Backspace up   D delete   R refresh",
             C_TEXT_DIM, GFX_TRANSPARENT, 1);
}

/* ---- Notepad ---- */

static struct window *open_notepad_empty(void)
{
    struct window *w = win_create(APP_NOTEPAD, "Notepad - new file", 560, 360);

    if (!w)
        return NULL;
    w->text = kzalloc(NOTE_CAP);
    if (!w->text) {
        win_close(w);
        return NULL;
    }
    strcpy(w->file, "/tmp/notes.txt");
    strcpy(w->status, "New file. Ctrl+S saves to /tmp/notes.txt");
    return w;
}

static void notepad_open(const char *path)
{
    struct window *w = open_notepad_empty();

    if (!w)
        return;
    strlcpy(w->file, path, sizeof w->file);
    ssize_t n = vfs_read_file(path, w->text, NOTE_CAP - 1);
    if (n < 0) {
        ksnprintf(w->status, sizeof w->status, "Cannot read: %s", kstrerror((int)n));
        n = 0;
    } else {
        strcpy(w->status, "Opened. Ctrl+S saves.");
    }
    w->len = (int)n;
    for (int i = 0; i < w->len; i++)
        if ((uint8_t)w->text[i] > 126 || ((uint8_t)w->text[i] < 32 && w->text[i] != '\n'))
            w->text[i] = '.';
    ksnprintf(w->title, sizeof w->title, "Notepad - %s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
}

static void note_cursor_pos(const struct window *w, int *line, int *col)
{
    int l = 0, c = 0;

    for (int i = 0; i < w->cur; i++) {
        if (w->text[i] == '\n') { l++; c = 0; } else c++;
    }
    *line = l;
    *col = c;
}

static int note_line_start(const struct window *w, int line)
{
    int i = 0;

    while (line > 0 && i < w->len) {
        if (w->text[i++] == '\n')
            line--;
    }
    return i;
}

static int note_line_len(const struct window *w, int start)
{
    int n = 0;

    while (start + n < w->len && w->text[start + n] != '\n')
        n++;
    return n;
}

static void note_insert(struct window *w, char c)
{
    if (w->len >= NOTE_CAP - 1)
        return;
    memmove(w->text + w->cur + 1, w->text + w->cur, (size_t)(w->len - w->cur));
    w->text[w->cur++] = c;
    w->len++;
}

static void note_key(struct window *w, int key, int mods)
{
    int line, col;

    note_cursor_pos(w, &line, &col);
    if (key == 19 || ((mods & MOD_CTRL) && (key == 's' || key == 'S'))) {     /* Ctrl+S */
        int r = vfs_write_file(w->file, w->text, (size_t)w->len);
        if (r < 0)
            ksnprintf(w->status, sizeof w->status, "Save failed: %s", kstrerror(r));
        else
            ksnprintf(w->status, sizeof w->status, "Saved %d bytes to %s", w->len, w->file);
        return;
    }
    switch (key) {
    case KEY_LEFT:  if (w->cur > 0) w->cur--; break;
    case KEY_RIGHT: if (w->cur < w->len) w->cur++; break;
    case KEY_HOME:  w->cur = note_line_start(w, line); break;
    case KEY_END: {
        int st = note_line_start(w, line);
        w->cur = st + note_line_len(w, st);
        break;
    }
    case KEY_UP:
        if (line > 0) {
            int st = note_line_start(w, line - 1);
            w->cur = st + MIN(col, note_line_len(w, st));
        }
        break;
    case KEY_DOWN: {
        int st = note_line_start(w, line + 1);
        if (st <= w->len && note_line_start(w, line + 1) > note_line_start(w, line))
            w->cur = st + MIN(col, note_line_len(w, st));
        break;
    }
    case '\b': case 127:
        if (w->cur > 0) {
            memmove(w->text + w->cur - 1, w->text + w->cur, (size_t)(w->len - w->cur));
            w->cur--;
            w->len--;
        }
        break;
    case KEY_DELETE:
        if (w->cur < w->len) {
            memmove(w->text + w->cur, w->text + w->cur + 1, (size_t)(w->len - w->cur - 1));
            w->len--;
        }
        break;
    case '\n': note_insert(w, '\n'); break;
    case '\t': for (int i = 0; i < 4; i++) note_insert(w, ' '); break;
    default:
        if (key >= 32 && key < 127)
            note_insert(w, (char)key);
        break;
    }
    note_cursor_pos(w, &line, &col);
    int rows = (w->ch - 20) / 16;
    if (line < w->top_line) w->top_line = line;
    if (line >= w->top_line + rows) w->top_line = line - rows + 1;
}

static void draw_notepad(struct window *w)
{
    struct gfx_surface *s = w->surf;
    int rows = (w->ch - 20) / 16;
    int cols = (w->cw - 8) / 8;
    int line, col;

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, GFX_WHITE);
    note_cursor_pos(w, &line, &col);
    int st = note_line_start(w, w->top_line);
    for (int r = 0; r < rows && st <= w->len; r++) {
        int n = note_line_len(w, st);
        for (int c = 0; c < n && c < cols; c++)
            gfx_char_tall(s, 4 + c * 8, 2 + r * 16, w->text[st + c], C_TEXT, GFX_TRANSPARENT);
        if (w->top_line + r == line && col <= cols)
            gfx_fill_rect(s, 4 + col * 8, 2 + r * 16, 2, 16, C_ACCENT);
        st += n + 1;
    }
    gfx_fill_rect(s, 0, w->ch - 18, w->cw, 18, C_LIGHT);
    char buf[100];
    ksnprintf(buf, sizeof buf, "Ln %d, Col %d   %s", line + 1, col + 1, w->status);
    gfx_text(s, 8, w->ch - 13, buf, C_TEXT_DIM, GFX_TRANSPARENT, 1);
}

/* ---- System info ---- */

static void bar(struct gfx_surface *s, int x, int y, int w, int pct, uint32_t col)
{
    gfx_fill_rect(s, x, y, w, 10, GFX_RGB(215, 220, 230));
    gfx_fill_rect(s, x, y, w * pct / 100, 10, col);
}

static void sysinfo_task_row(struct task *t, void *ctx)
{
    struct { struct gfx_surface *s; int *y; int max; } *c = ctx;
    char line[64];

    if (*c->y > c->max)
        return;
    ksnprintf(line, sizeof line, "%3d  %-12s %-8s", t->pid, t->name,
              t->state == TASK_RUNNING ? "running" : t->state == TASK_READY ? "ready" :
              t->state == TASK_SLEEPING ? "sleeping" : t->state == TASK_BLOCKED ? "blocked" : "zombie");
    gfx_text(c->s, 16, *c->y, line, C_TEXT, GFX_TRANSPARENT, 1);
    *c->y += 12;
}

static void draw_sysinfo(struct window *w)
{
    struct gfx_surface *s = w->surf;
    char line[96], vendor[13], brand[49];
    int y = 10;

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, C_WIN_BG);
    cpu_vendor(vendor);
    gfx_text(s, 12, y, "MyOS " MYOS_VERSION, C_ACCENT, GFX_TRANSPARENT, 2);
    y += 26;
    if (cpu_brand(brand)) {
        const char *b = brand;
        while (*b == ' ') b++;
        ksnprintf(line, sizeof line, "CPU: %s", b);
    } else {
        ksnprintf(line, sizeof line, "CPU: %s", vendor);
    }
    gfx_text(s, 12, y, line, C_TEXT, GFX_TRANSPARENT, 1);
    y += 14;
    ksnprintf(line, sizeof line, "Screen: %dx%d   Boot loader: %s", sw, sh, boot_info.loader);
    gfx_text(s, 12, y, line, C_TEXT, GFX_TRANSPARENT, 1);
    y += 14;
    uint32_t sec = timer_uptime_sec();
    ksnprintf(line, sizeof line, "Uptime: %u:%02u:%02u   Scheduler: %s", sec / 3600, (sec / 60) % 60,
              sec % 60, sched_get_policy()->name);
    gfx_text(s, 12, y, line, C_TEXT, GFX_TRANSPARENT, 1);
    y += 22;

    uint32_t total = pmm_total_pages(), used = pmm_used_pages();
    ksnprintf(line, sizeof line, "Memory: %u of %u MiB used", used / 256, total / 256);
    gfx_text(s, 12, y, line, C_TEXT, GFX_TRANSPARENT, 1);
    bar(s, 12, y + 12, w->cw - 24, total ? (int)(used * 100 / total) : 0, C_ACCENT);
    y += 32;

    struct heap_stats h;
    heap_get_stats(&h);
    ksnprintf(line, sizeof line, "Kernel heap: %u of %u KiB, %u allocations", h.used_bytes / 1024,
              h.total_bytes / 1024, h.alloc_count);
    gfx_text(s, 12, y, line, C_TEXT, GFX_TRANSPARENT, 1);
    bar(s, 12, y + 12, w->cw - 24, h.total_bytes ? (int)(h.used_bytes * 100ull / h.total_bytes) : 0,
        GFX_RGB(60, 170, 110));
    y += 36;

    gfx_text(s, 12, y, "Tasks:", C_TEXT, GFX_TRANSPARENT, 1);
    y += 14;
    struct { struct gfx_surface *s; int *y; int max; } ctx = { s, &y, w->ch - 14 };
    sched_for_each(sysinfo_task_row, &ctx);
}

/* ---- About ---- */

static void draw_about(struct window *w)
{
    struct gfx_surface *s = w->surf;

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, C_WIN_BG);
    gfx_draw_logo(s, 20, 20, 72);
    gfx_text(s, 108, 28, "MyOS", C_TEXT, GFX_TRANSPARENT, 3);
    gfx_text(s, 110, 58, "Version " MYOS_VERSION " \"" MYOS_CODENAME "\"", C_TEXT_DIM, GFX_TRANSPARENT, 1);
    gfx_text(s, 20, 108, "A 32-bit operating system written from", C_TEXT, GFX_TRANSPARENT, 1);
    gfx_text(s, 20, 122, "scratch in C and assembly.", C_TEXT, GFX_TRANSPARENT, 1);
    gfx_text(s, 20, 146, "Memory manager, preemptive scheduler,", C_TEXT, GFX_TRANSPARENT, 1);
    gfx_text(s, 20, 160, "virtual file system, ATA disks, GUI.", C_TEXT, GFX_TRANSPARENT, 1);
    gfx_text(s, 20, 190, "github.com/arshc0der/myOS", C_ACCENT, GFX_TRANSPARENT, 1);
}

static struct window *open_app(enum app_type t)
{
    for (int i = 0; i < MAX_WINDOWS; i++)             /* single instance apps */
        if (wins[i].used && wins[i].type == t && (t == APP_SYSINFO || t == APP_ABOUT || t == APP_FILES)) {
            bring_to_front(i);
            return &wins[i];
        }
    switch (t) {
    case APP_TERMINAL: return open_terminal();
    case APP_FILES:    return open_files();
    case APP_SYSINFO:  return win_create(APP_SYSINFO, "System Info", 460, 330);
    case APP_NOTEPAD:  return open_notepad_empty();
    case APP_ABOUT:    return win_create(APP_ABOUT, "About MyOS", 340, 230);
    }
    return NULL;
}

static void draw_window_contents(struct window *w)
{
    switch (w->type) {
    case APP_TERMINAL: draw_terminal(w); break;
    case APP_FILES:    draw_files(w); break;
    case APP_SYSINFO:  draw_sysinfo(w); break;
    case APP_NOTEPAD:  draw_notepad(w); break;
    case APP_ABOUT:    draw_about(w); break;
    }
}

/* ------------------------------------------------------------------ */
/* Desktop drawing                                                     */
/* ------------------------------------------------------------------ */

struct icon { const char *label; char glyph; enum app_type app; uint32_t color, color2; };

static const struct icon icons[] = {
    { "Terminal", '>', APP_TERMINAL, GFX_RGB(70, 74, 86),   GFX_RGB(20, 22, 28) },
    { "Files",    'F', APP_FILES,    GFX_RGB(90, 190, 250), GFX_RGB(20, 120, 230) },
    { "Info",     'i', APP_SYSINFO,  GFX_RGB(120, 220, 140), GFX_RGB(30, 160, 90) },
    { "Notepad",  'N', APP_NOTEPAD,  GFX_RGB(255, 220, 90), GFX_RGB(240, 160, 30) },
    { "About",    '?', APP_ABOUT,    GFX_RGB(210, 140, 250), GFX_RGB(140, 70, 220) },
};
#define ICON_N 5
#define DOCK_ICON 52
#define DOCK_GAP 14

static void dock_rect(int *x, int *y, int *w, int *h)
{
    *w = ICON_N * DOCK_ICON + (ICON_N - 1) * DOCK_GAP + 32;
    *h = 66;
    *x = (sw - *w) / 2;
    *y = sh - *h - 8;
}

static int dock_icon_x(int i)
{
    int x, y, w, h;
    dock_rect(&x, &y, &w, &h);
    return x + 16 + i * (DOCK_ICON + DOCK_GAP);
}

static bool app_running(enum app_type t)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used && wins[i].type == t)
            return true;
    return false;
}

static void draw_dock(struct gfx_surface *s)
{
    int x, y, w, h;

    dock_rect(&x, &y, &w, &h);
    gfx_fill_rect_alpha(s, x, y, w, h, GFX_RGB(255, 255, 255), 70);
    gfx_fill_round_rect(s, x, y, w, h, 18, gfx_blend(gfx_get_pixel(s, x + w / 2, y + 2), GFX_RGB(255, 255, 255), 90));
    for (int i = 0; i < ICON_N; i++) {
        int ix = dock_icon_x(i);
        bool hot = mx >= ix - 4 && mx < ix + DOCK_ICON + 4 && my >= y && my < y + h;
        int sz = hot ? 60 : DOCK_ICON;
        int dx = ix - (sz - DOCK_ICON) / 2, dy = y + 7 - (hot ? 8 : 0);
        gfx_fill_round_rect(s, dx, dy, sz, sz, 13, icons[i].color2);
        gfx_fill_round_rect(s, dx, dy, sz, sz * 6 / 10, 13, icons[i].color);
        gfx_char(s, dx + sz / 2 - 8, dy + sz / 2 - 8, icons[i].glyph, GFX_WHITE, GFX_TRANSPARENT, 2);
        if (app_running(icons[i].app))
            gfx_fill_circle(s, ix + DOCK_ICON / 2, y + h - 6, 2, GFX_RGB(40, 40, 50));
        if (hot) {
            int lw = gfx_text_width(icons[i].label, 1) + 16;
            int lx = ix + DOCK_ICON / 2 - lw / 2, ly = y - 34;
            gfx_fill_round_rect(s, lx, ly, lw, 20, 6, GFX_RGB(60, 60, 68));
            gfx_text(s, lx + 8, ly + 6, icons[i].label, GFX_WHITE, GFX_TRANSPARENT, 1);
        }
    }
}

static void draw_window(struct gfx_surface *s, struct window *w, bool active)
{
    int ow = outer_w(w), oh = outer_h(w);

    for (int k = 3; k >= 0; k--)                                      /* soft shadow */
        gfx_fill_round_rect(s, w->x - k * 3, w->y + 6 - k, ow + k * 6, oh + k * 4, 14,
                            gfx_blend(gfx_get_pixel(s, w->x + ow / 2, MIN(sh - 1, w->y + oh + 4)), GFX_BLACK, 22));
    gfx_fill_round_rect(s, w->x, w->y, ow, oh, 10, C_WIN_BG);
    uint32_t tcol = active ? GFX_RGB(232, 232, 236) : GFX_RGB(244, 244, 247);
    gfx_fill_round_rect(s, w->x, w->y, ow, TITLE_H + 12, 10, tcol);
    gfx_fill_rect(s, w->x, w->y + TITLE_H, ow, 12, C_WIN_BG);
    gfx_hline(s, w->x, w->y + TITLE_H - 1, ow, GFX_RGB(205, 205, 210));
    /* traffic lights */
    gfx_fill_circle(s, w->x + 16, w->y + 14, 6, active ? GFX_RGB(255, 95, 87) : GFX_RGB(200, 200, 204));
    gfx_fill_circle(s, w->x + 36, w->y + 14, 6, active ? GFX_RGB(254, 188, 46) : GFX_RGB(200, 200, 204));
    gfx_fill_circle(s, w->x + 56, w->y + 14, 6, active ? GFX_RGB(40, 200, 64) : GFX_RGB(200, 200, 204));
    int tw = gfx_text_width(w->title, 1);
    gfx_text(s, w->x + (ow - tw) / 2, w->y + 10, w->title,
             active ? GFX_RGB(40, 40, 46) : GFX_RGB(150, 150, 158), GFX_TRANSPARENT, 1);
    draw_window_contents(w);
    gfx_blit(s, w->x + 1, w->y + TITLE_H, w->surf, 0, 0, w->cw, w->ch);
}

static const char *menu_labels[] = {
    "Terminal", "Files", "System Info", "Notepad", "About MyOS",
    "Exit to text console", "Shut down", "Restart",
};
#define MENU_COUNT 8

static void menu_rect(int *x, int *y, int *w, int *h)
{
    *w = MENU_W;
    *h = MENU_COUNT * MENU_ITEM_H + 10;
    *x = 8;
    *y = MENUBAR_H + 2;
}

static void draw_menu(struct gfx_surface *s)
{
    int x, y, w, h;

    menu_rect(&x, &y, &w, &h);
    gfx_fill_round_rect(s, x + 2, y + 4, w, h, 10, gfx_blend(gfx_get_pixel(s, x + 4, y + h + 6), GFX_BLACK, 40));
    gfx_fill_round_rect(s, x, y, w, h, 10, GFX_RGB(244, 244, 248));
    for (int i = 0; i < MENU_COUNT; i++) {
        int iy = y + 5 + i * MENU_ITEM_H;
        bool hot = mx >= x && mx < x + w && my >= iy && my < iy + MENU_ITEM_H;
        if (hot)
            gfx_fill_round_rect(s, x + 5, iy, w - 10, MENU_ITEM_H - 2, 6, C_ACCENT);
        gfx_text(s, x + 16, iy + 9, menu_labels[i], hot ? GFX_WHITE : C_TEXT, GFX_TRANSPARENT, 1);
        if (i == 4)
            gfx_hline(s, x + 12, iy + MENU_ITEM_H - 1, w - 24, GFX_RGB(210, 210, 216));
    }
}

static void draw_menubar(struct gfx_surface *s)
{
    gfx_fill_rect_alpha(s, 0, 0, sw, MENUBAR_H, GFX_RGB(246, 246, 250), 215);
    gfx_hline(s, 0, MENUBAR_H - 1, sw, GFX_RGB(200, 200, 208));
    if (menu_open)
        gfx_fill_round_rect(s, 4, 3, 36, MENUBAR_H - 6, 6, C_ACCENT);
    gfx_draw_logo(s, 14, 6, 16);
    struct window *f = focused();
    gfx_text(s, 52, 10, f ? f->title : "Desktop", C_TEXT, GFX_TRANSPARENT, 1);
    if (!have_mouse || ptr_mode)
        gfx_text(s, sw / 2 - 150, 10, ptr_mode ? "POINTER MODE: arrows move, Enter clicks, F2/Esc exits"
                 : "No mouse? Press F2: arrows move pointer", GFX_RGB(150, 60, 60), GFX_TRANSPARENT, 1);

    uint32_t tm[6];
    char clock[24];
    rtc_read(tm);
    ksnprintf(clock, sizeof clock, "%02u:%02u:%02u", tm[3], tm[4], tm[5]);
    gfx_text(s, sw - gfx_text_width(clock, 1) - 14, 10, clock, C_TEXT, GFX_TRANSPARENT, 1);
}

static const char *cursor_shape[19] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ", "X    X..X   ",
    "     X..X   ", "      X..X  ", "      X..X  ", "       XX   ",
};

static void draw_cursor(struct gfx_surface *s)
{
    for (int j = 0; j < 19; j++)
        for (int i = 0; i < 12; i++) {
            char c = cursor_shape[j][i];
            if (c == 'X')
                gfx_pixel(s, mx + i, my + j, GFX_BLACK);
            else if (c == '.')
                gfx_pixel(s, mx + i, my + j, GFX_WHITE);
        }
}

static void build_wallpaper(void)
{
    wallpaper = gfx_surface_create(sw, sh);
    if (!wallpaper)
        return;
    gfx_gradient_v(wallpaper, 0, 0, sw, sh, C_BG_TOP, C_BG_BOTTOM);
    /* soft watermark logo */
    int size = MIN(sw, sh) / 3;
    struct gfx_surface *tmp = gfx_surface_create(size, size);
    if (tmp) {
        for (int i = 0; i < size * size; i++)
            tmp->px[i] = 0xFF000000u;                  /* marker: untouched */
        gfx_draw_logo(tmp, 0, 0, size);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                uint32_t p = tmp->px[y * size + x];
                if (p == 0xFF000000u)
                    continue;
                int wx = sw - size - 40 + x, wy = (sh - TASKBAR_H - size) / 2 + y;
                gfx_pixel(wallpaper, wx, wy, gfx_blend(gfx_get_pixel(wallpaper, wx, wy), p, 40));
            }
        gfx_surface_free(tmp);
    }
}

static void render(void)
{
    struct gfx_surface *s = gfx_screen();

    if (wallpaper)
        gfx_blit(s, 0, 0, wallpaper, 0, 0, sw, sh);
    else
        gfx_fill_rect(s, 0, 0, sw, sh, C_BG_TOP);
    for (int i = 0; i < nz; i++)
        draw_window(s, &wins[zorder[i]], i == nz - 1);
    draw_dock(s);
    draw_menubar(s);
    if (menu_open)
        draw_menu(s);
    draw_cursor(s);
    gfx_flush_all();
}

/* ------------------------------------------------------------------ */
/* Splash screen                                                       */
/* ------------------------------------------------------------------ */

void gui_splash(void)
{
    if (!gfx_available())
        return;
    struct gfx_surface *s = gfx_screen();
    int w = gfx_width(), h = gfx_height();
    int size = MIN(w, h) / 4;
    int lx = (w - size) / 2, ly = h / 2 - size / 2 - 30;
    static const char *steps[] = {
        "Starting the memory manager...", "Mounting file systems...",
        "Loading input drivers...", "Starting the desktop...",
    };

    for (int step = 0; step <= 4; step++) {
        gfx_gradient_v(s, 0, 0, w, h, GFX_RGB(8, 14, 34), GFX_RGB(20, 52, 110));
        gfx_draw_logo(s, lx, ly, size);
        int tw = gfx_text_width("MyOS", 4);
        gfx_text(s, (w - tw) / 2, ly + size + 22, "MyOS", GFX_WHITE, GFX_TRANSPARENT, 4);
        int bw = size * 2, bx = (w - bw) / 2, by = ly + size + 80;
        gfx_fill_round_rect(s, bx, by, bw, 8, 4, GFX_RGB(40, 52, 84));
        if (step > 0)
            gfx_fill_round_rect(s, bx, by, bw * step / 4, 8, 4, GFX_RGB(80, 160, 255));
        const char *msg = step < 4 ? steps[step] : "Welcome";
        gfx_text(s, (w - gfx_text_width(msg, 1)) / 2, by + 20, msg, GFX_RGB(170, 190, 230), GFX_TRANSPARENT, 1);
        gfx_flush_all();
        timer_sleep_ms(step < 4 ? 260 : 350);
    }
}

/* ------------------------------------------------------------------ */
/* Input handling                                                      */
/* ------------------------------------------------------------------ */

static int drag_idx = -1, drag_dx, drag_dy;
static uint32_t last_click_tick;
static int last_click_row = -1;

static int hit_window(int x, int y)
{
    for (int i = nz - 1; i >= 0; i--) {
        struct window *w = &wins[zorder[i]];
        if (x >= w->x && x < w->x + outer_w(w) && y >= w->y && y < w->y + outer_h(w))
            return zorder[i];
    }
    return -1;
}


/* Automatic test: boot with "gui guitest" and watch the serial port. */
static void push_key(int k, int mods)
{
    struct input_event ev = { .type = INPUT_KEY, .key = k, .mods = (uint8_t)mods };
    input_push(&ev);
}

static void guitest_task(void *arg)
{
    (void)arg;
    const char *cmd = "echo hi\n";
    timer_sleep_ms(2500);
    for (const char *c = cmd; *c; c++) {
        push_key(*c, 0);
        timer_sleep_ms(100);
    }
    struct input_event m = { .type = INPUT_MOUSE, .dx = 40, .dy = 20 };
    input_push(&m);
    timer_sleep_ms(1500);
    push_key('q', MOD_ALT);
}

static void guitest_dump(void)
{
    char line[200];
    if (!term_win)
        return;
    serial_write("GUITEST-TERM-BEGIN\n");
    for (int r = 0; r < term_win->tg.rows; r++) {
        int n = 0;
        for (int c = 0; c < term_win->tg.cols && n < 190; c++) {
            char ch = (char)tg_cell(&term_win->tg, r, c)->ch;
            line[n++] = ch ? ch : ' ';
        }
        while (n > 0 && line[n - 1] == ' ')
            n--;
        line[n] = '\0';
        serial_write(line);
        serial_write("\n");
    }
    serial_write("GUITEST-TERM-END\n");
    {
        char b[64];
        ksnprintf(b, sizeof b, "GUITEST mouse=%d,%d have_mouse=%d\n", mx, my, (int)have_mouse);
        serial_write(b);
    }
}

static void quit_gui(void) { running = false; }

static void menu_action(int i)
{
    menu_open = false;
    switch (i) {
    case 0: open_app(APP_TERMINAL); break;
    case 1: open_app(APP_FILES); break;
    case 2: open_app(APP_SYSINFO); break;
    case 3: open_app(APP_NOTEPAD); break;
    case 4: open_app(APP_ABOUT); break;
    case 5: quit_gui(); break;
    case 6: system_poweroff();
    case 7: system_reboot();
    }
}

static void cycle_windows(void)
{
    if (nz > 1) {
        int back = zorder[0];
        bring_to_front(back);
    }
}

static void mouse_click(int x, int y)
{
    int mxx, myy, mw, mh;

    if (menu_open) {
        menu_rect(&mxx, &myy, &mw, &mh);
        if (x >= mxx && x < mxx + mw && y >= myy && y < myy + mh) {
            int i = (y - myy - 5) / MENU_ITEM_H;
            if (i >= 0 && i < MENU_COUNT)
                menu_action(i);
            return;
        }
        menu_open = false;
        if (y >= MENUBAR_H)
            return;
    }
    if (y < MENUBAR_H) {                                    /* menu bar */
        if (x < 44)
            menu_open = !menu_open;
        return;
    }
    {                                                       /* dock */
        int dx, dy, dw, dh;
        dock_rect(&dx, &dy, &dw, &dh);
        if (x >= dx && x < dx + dw && y >= dy - 10 && y < dy + dh) {
            for (int i = 0; i < ICON_N; i++) {
                int ix = dock_icon_x(i);
                if (x >= ix - 4 && x < ix + DOCK_ICON + 4)
                    open_app(icons[i].app);
            }
            return;
        }
    }
    int idx = hit_window(x, y);
    if (idx < 0)
        return;
    struct window *w = &wins[idx];
    bring_to_front(idx);
    int lx = x - w->x, ly = y - w->y;
    if (ly < TITLE_H) {
        if (lx < 70) {                                      /* traffic lights */
            if (lx < 26)
                win_close(w);
            return;
        }
        drag_idx = idx;
        drag_dx = lx;
        drag_dy = ly;
        return;
    }
    /* click inside the client area */
    int cy = ly - TITLE_H;
    if (w->type == APP_FILES && cy >= 26) {
        int row = (cy - 26) / 16 + w->top;
        if (row < w->nent) {
            w->sel = row;
            uint32_t now = timer_ticks();
            if (row == last_click_row && now - last_click_tick < 50)
                files_activate(w, row);                     /* double click */
            last_click_row = row;
            last_click_tick = now;
        }
    }
}

static void handle_mouse(const struct input_event *ev)
{
    have_mouse = true;
    int ax = ev->dx, ay = ev->dy;
    if (ax > 6 || ax < -6) ax *= 2;
    if (ay > 6 || ay < -6) ay *= 2;
    mx = MAX(0, MIN(sw - 1, mx + ax * 3 / 2));
    my = MAX(0, MIN(sh - 1, my + ay * 3 / 2));
    bool left_now = ev->buttons & MOUSE_LEFT, left_before = mbuttons & MOUSE_LEFT;

    if (left_now && !left_before)
        mouse_click(mx, my);
    if (!left_now)
        drag_idx = -1;
    if (left_now && drag_idx >= 0 && wins[drag_idx].used) {
        wins[drag_idx].x = MAX(-wins[drag_idx].cw + 60, MIN(sw - 60, mx - drag_dx));
        wins[drag_idx].y = MAX(MENUBAR_H, MIN(sh - TASKBAR_H - TITLE_H, my - drag_dy));
    }
    mbuttons = ev->buttons;
}

static void handle_key(const struct input_event *ev)
{
    int k = ev->key;

    if (ev->mods & MOD_ALT) {                                /* global shortcuts */
        switch (k) {
        case 't': case 'T': open_app(APP_TERMINAL); return;
        case 'f': case 'F': open_app(APP_FILES); return;
        case 'i': case 'I': open_app(APP_SYSINFO); return;
        case 'n': case 'N': open_app(APP_NOTEPAD); return;
        case 'a': case 'A': open_app(APP_ABOUT); return;
        case 'm': case 'M': menu_open = !menu_open; return;
        case 'q': case 'Q': quit_gui(); return;
        case 'w': case 'W': case KEY_F(4):
            if (focused())
                win_close(focused());
            return;
        case '\t': cycle_windows(); return;
        /* keyboard pointer: Alt+arrows move it (Shift = faster), Alt+Enter clicks */
        case KEY_LEFT:  mx = MAX(0, mx - ((ev->mods & MOD_SHIFT) ? 64 : 16)); return;
        case KEY_RIGHT: mx = MIN(sw - 1, mx + ((ev->mods & MOD_SHIFT) ? 64 : 16)); return;
        case KEY_UP:    my = MAX(0, my - ((ev->mods & MOD_SHIFT) ? 64 : 16)); return;
        case KEY_DOWN:  my = MIN(sh - 1, my + ((ev->mods & MOD_SHIFT) ? 64 : 16)); return;
        case '\n': case ' ': mouse_click(mx, my); return;
        }
    }
    if (k == KEY_F(2)) {                                     /* toggle keyboard pointer */
        ptr_mode = !ptr_mode;
        return;
    }
    if (ptr_mode) {
        int step = (ev->mods & MOD_SHIFT) ? 64 : 16;
        switch (k) {
        case KEY_LEFT:  mx = MAX(0, mx - step); return;
        case KEY_RIGHT: mx = MIN(sw - 1, mx + step); return;
        case KEY_UP:    my = MAX(0, my - step); return;
        case KEY_DOWN:  my = MIN(sh - 1, my + step); return;
        case '\n': case ' ': mouse_click(mx, my); return;
        case 27: ptr_mode = false; return;
        }
    }
    if (menu_open) {
        if (k == 27)
            menu_open = false;
        return;
    }
    struct window *w = focused();
    if (!w)
        return;
    switch (w->type) {
    case APP_TERMINAL: kq_push(k); break;
    case APP_FILES:    files_key(w, k); break;
    case APP_NOTEPAD:  note_key(w, k, ev->mods); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

bool gui_supported(void)
{
    return gfx_available();
}

void gui_run(void)
{
    if (!gfx_available()) {
        kprintf("gui: no graphics framebuffer on this machine/boot mode.\n");
        return;
    }
    sw = gfx_width();
    sh = gfx_height();
    memset(wins, 0, sizeof wins);
    nz = 0;
    menu_open = false;
    ptr_mode = false;
    running = true;
    term_win = NULL;
    mx = sw / 2;
    my = sh / 2;
    mbuttons = 0;
    have_mouse = mouse_present();                       /* only used for hints */
    drag_idx = -1;

    serial_write("GUI: splash\n");
    gui_splash();
    serial_write("GUI: wallpaper\n");
    build_wallpaper();
    serial_write("GUI: wallpaper done\n");
    input_flush();

    console_set_backend(&gui_backend);                       /* shell output -> Terminal */
    console_set_key_source(term_getkey);
    open_terminal();
    if (term_win) {
        term_win->x = 40;
        term_win->y = MENUBAR_H + 16;
    }
    open_app(APP_SYSINFO);
    if (nz > 1) {
        struct window *info = &wins[zorder[nz - 1]];
        info->x = MAX(20, sw - outer_w(info) - 30);
        info->y = MENUBAR_H + 40;
        bring_to_front(win_index(term_win));
    }

    if (boot_has_arg("guitest"))
        task_create("guitest", guitest_task, NULL);

    serial_write("GUI: loop\n");
    uint32_t last_sec = ~0u;
    while (running) {
        struct input_event ev;
        bool redraw = false;
        int r = input_wait(&ev, 100);

        while (r == 0) {
            if (ev.type == INPUT_MOUSE)
                handle_mouse(&ev);
            else if (ev.type == INPUT_KEY)
                handle_key(&ev);
            redraw = true;
            r = input_poll(&ev) ? 0 : -1;
        }
        /* the Terminal's shell ended (exit) -> close that window */
        while (kq_head != kq_tail && keyq[kq_tail] == -2) {
            kq_tail = (kq_tail + 1) % KQ;
            if (term_win) {
                term_win->shell_pid = 0;
                win_close(term_win);
            }
            redraw = true;
        }
        if (term_dirty) {
            term_dirty = false;
            redraw = true;
        }
        uint32_t sec = timer_uptime_sec();
        if (sec != last_sec) {
            last_sec = sec;
            redraw = true;
        }
        if (redraw) {
            render();
            if (boot_has_arg("guitest")) serial_write("GUI: frame\n");
        }
    }

    if (boot_has_arg("guitest"))
        guitest_dump();
    /* leave: restore the plain text console */
    while (nz > 0)
        win_close(&wins[zorder[nz - 1]]);
    console_set_key_source(NULL);
    console_set_backend(&fbcon_console);
    gfx_surface_free(wallpaper);
    wallpaper = NULL;
    input_flush();
}
