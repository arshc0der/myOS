/* files.c - the Files app (a small Finder): sidebar, folder list, open text files in Notepad. */
#include "guiapi.h"
#include "input.h"
#include "vfs.h"
#include "heap.h"
#include "timer.h"
#include "kprintf.h"
#include "kstring.h"

#define MAXENT 128
#define SIDE_W 130
#define TOOL_H 34
#define ROW_H 22

struct files {
    char path[VFS_PATH_MAX];
    struct dirent ent[MAXENT];
    int nent, sel, top;
    uint32_t last_click;
    int last_row;
};

static const struct { const char *label, *path; } places[] = {
    { "Macintosh HD", "/" }, { "tmp", "/tmp" }, { "mnt", "/mnt" }, { "dev", "/dev" },
};

static int ent_cmp(const struct dirent *a, const struct dirent *b)
{
    bool da = a->type == VFS_DIR, db = b->type == VFS_DIR;

    if (da != db)
        return da ? -1 : 1;
    return strcmp(a->name, b->name);
}

static void load(struct files *f)
{
    struct dirent de;

    f->nent = 0;
    for (uint32_t i = 0; f->nent < MAXENT && vfs_readdir(f->path, i, &de) == 0; i++)
        f->ent[f->nent++] = de;
    for (int i = 1; i < f->nent; i++) {
        struct dirent k = f->ent[i];
        int j = i - 1;
        while (j >= 0 && ent_cmp(&f->ent[j], &k) > 0) {
            f->ent[j + 1] = f->ent[j];
            j--;
        }
        f->ent[j + 1] = k;
    }
    f->sel = f->nent ? 0 : -1;
    f->top = 0;
}

static void go(struct files *f, const char *path)
{
    char abs[VFS_PATH_MAX];
    struct vfs_stat st;

    if (vfs_normalize(path, abs, sizeof abs) < 0 || vfs_stat(abs, &st) < 0 || st.type != VFS_DIR)
        return;
    strlcpy(f->path, abs, sizeof f->path);
    load(f);
}

static void parent(struct files *f)
{
    char p[VFS_PATH_MAX];

    strlcpy(p, f->path, sizeof p);
    char *slash = strrchr(p, '/');
    if (!slash)
        return;
    if (slash == p)
        p[1] = '\0';
    else
        *slash = '\0';
    go(f, p);
}

static void full(const struct files *f, int i, char *out, size_t n)
{
    ksnprintf(out, n, "%s/%s", strcmp(f->path, "/") ? f->path : "", f->ent[i].name);
}

static void activate(struct files *f, int i)
{
    char p[VFS_PATH_MAX];

    if (i < 0 || i >= f->nent)
        return;
    full(f, i, p, sizeof p);
    if (f->ent[i].type == VFS_DIR)
        go(f, p);
    else if (f->ent[i].type == VFS_FILE)
        notepad_open_file(p);
}

static int rows(const struct gui_window *w) { return (w->ch - TOOL_H - 22) / ROW_H; }

static void files_open(struct gui_window *w)
{
    struct files *f = kzalloc(sizeof *f);

    if (!f) {
        gui_close(w);
        return;
    }
    w->priv = f;
    f->last_row = -1;
    go(f, "/");
}

static void files_close(struct gui_window *w) { kfree(w->priv); w->priv = NULL; }

static void files_draw(struct gui_window *w)
{
    struct files *f = w->priv;
    struct gfx_surface *s = w->surf;
    const struct gui_theme *t = gui_th;

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, t->win);
    gfx_fill_rect(s, 0, 0, SIDE_W, w->ch, t->side);
    gfx_vline(s, SIDE_W, 0, w->ch, t->line);
    gfx_text(s, 14, 12, "Favorites", t->dim, GFX_TRANSPARENT, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(places); i++)
        gui_sidebar_row(s, 0, 28 + (int)i * 26, SIDE_W, places[i].label, strcmp(f->path, places[i].path) == 0);

    /* toolbar */
    gfx_fill_rect(s, SIDE_W + 1, 0, w->cw - SIDE_W - 1, TOOL_H, t->title);
    gfx_hline(s, SIDE_W + 1, TOOL_H - 1, w->cw - SIDE_W - 1, t->line);
    gui_button(s, SIDE_W + 10, 5, 30, 24, "<", false);
    gfx_text(s, SIDE_W + 54, 13, f->path, t->text, GFX_TRANSPARENT, 1);

    int r = rows(w);
    for (int i = 0; i < r && f->top + i < f->nent; i++) {
        const struct dirent *e = &f->ent[f->top + i];
        int y = TOOL_H + 4 + i * ROW_H;
        bool sel = f->top + i == f->sel;
        if (sel)
            gfx_fill_round_rect(s, SIDE_W + 6, y, w->cw - SIDE_W - 12, ROW_H - 2, 6, t->sel);
        uint32_t fg = sel ? t->sel_text : t->text;
        if (e->type == VFS_DIR) {
            gfx_fill_round_rect(s, SIDE_W + 16, y + 3, 7, 4, 1, fg);
            gfx_fill_round_rect(s, SIDE_W + 16, y + 5, 15, 11, 2, fg);
        } else {
            gfx_rect(s, SIDE_W + 19, y + 3, 10, 13, t->dim);
        }
        gfx_text(s, SIDE_W + 40, y + 7, e->name, fg, GFX_TRANSPARENT, 1);
        if (e->type == VFS_FILE || e->type == VFS_BLOCK) {
            char sz[16];
            ksnprintf(sz, sizeof sz, "%u", e->size);
            gfx_text(s, w->cw - 14 - gfx_text_width(sz, 1), y + 7, sz, t->dim, GFX_TRANSPARENT, 1);
        }
    }
    if (!f->nent)
        gfx_text(s, SIDE_W + 20, TOOL_H + 20, "This folder is empty", t->dim, GFX_TRANSPARENT, 1);
    char st[48];
    ksnprintf(st, sizeof st, "%d items", f->nent);
    gfx_hline(s, SIDE_W + 1, w->ch - 20, w->cw - SIDE_W - 1, t->line);
    gfx_text(s, SIDE_W + 12, w->ch - 14, st, t->dim, GFX_TRANSPARENT, 1);
    gfx_text(s, w->cw - 12 - gfx_text_width("Enter open  N new folder  D delete", 1), w->ch - 14,
             "Enter open  N new folder  D delete", t->dim, GFX_TRANSPARENT, 1);
}

static void files_key(struct gui_window *w, int key, int mods)
{
    struct files *f = w->priv;
    int r = rows(w);
    (void)mods;

    switch (key) {
    case KEY_UP:   if (f->sel > 0) f->sel--; break;
    case KEY_DOWN: if (f->sel < f->nent - 1) f->sel++; break;
    case KEY_PGUP: f->sel = MAX(0, f->sel - r); break;
    case KEY_PGDN: f->sel = MIN(f->nent - 1, f->sel + r); break;
    case KEY_HOME: f->sel = f->nent ? 0 : -1; break;
    case KEY_END:  f->sel = f->nent - 1; break;
    case '\n':     activate(f, f->sel); break;
    case '\b': case KEY_LEFT: parent(f); break;
    case 'r': case KEY_F(5): load(f); break;
    case 'n': {
        char p[VFS_PATH_MAX];
        for (int i = 1; i < 100; i++) {
            ksnprintf(p, sizeof p, "%s/untitled folder %d", strcmp(f->path, "/") ? f->path : "", i);
            if (vfs_mkdir(p) == 0)
                break;
        }
        load(f);
        break;
    }
    case 'd':
        if (f->sel >= 0 && f->sel < f->nent) {
            char p[VFS_PATH_MAX];
            int s = f->sel;
            full(f, s, p, sizeof p);
            vfs_unlink(p);
            load(f);
            f->sel = MIN(s, f->nent - 1);
        }
        break;
    }
    if (f->sel >= 0) {
        if (f->sel < f->top) f->top = f->sel;
        if (f->sel >= f->top + r) f->top = f->sel - r + 1;
    }
    gui_redraw();
}

static void files_click(struct gui_window *w, int x, int y, int buttons)
{
    struct files *f = w->priv;
    (void)buttons;

    if (x < SIDE_W) {
        int i = (y - 28) / 26;
        if (y >= 28 && i >= 0 && i < (int)ARRAY_SIZE(places))
            go(f, places[i].path);
    } else if (y < TOOL_H) {
        if (x >= SIDE_W + 10 && x < SIDE_W + 40)
            parent(f);
    } else {
        int row = (y - TOOL_H - 4) / ROW_H + f->top;
        if (row >= 0 && row < f->nent) {
            f->sel = row;
            uint32_t now = timer_ticks();
            if (row == f->last_row && now - f->last_click < 50)
                activate(f, row);
            f->last_row = row;
            f->last_click = now;
        }
    }
    gui_redraw();
}

static void files_icon(struct gfx_surface *s, int x, int y, int sz, uint32_t g, uint32_t tile)
{
#define P(f) ((sz * (f)) / 100)
    gfx_fill_round_rect(s, x + P(20), y + P(26), P(26), P(14), P(4), g);
    gfx_fill_round_rect(s, x + P(18), y + P(34), P(64), P(40), P(8), g);
    gfx_fill_rect(s, x + P(18), y + P(46), P(64), MAX(P(3), 1), tile);
#undef P
}

static const struct gui_app files_app = {
    .name = "Files", .title = "Files", .cw = 620, .ch = 380, .single = true, .dock = 20,
    .open = files_open, .close = files_close, .draw = files_draw, .key = files_key,
    .click = files_click, .icon = files_icon,
};
GUI_APP(files_app);
