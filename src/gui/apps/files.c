/*
 * files.c - the Files app (a small Finder).
 *
 * Browse folders (icon or list view), open text files in Notepad, create
 * folders and files, rename, copy / cut / paste, delete, and a right-click
 * menu. All the real work is done by fileops.c, so this file is only the
 * user interface. It is also a good example of an app that uses the menu bar
 * (File / Edit / View), popup menus, dialogs and the clipboard from guiapi.h.
 */
#include "guiapi.h"
#include "input.h"
#include "vfs.h"
#include "fileops.h"
#include "heap.h"
#include "kerror.h"
#include "kprintf.h"
#include "kstring.h"

#define MAXENT 192
#define SIDE_W 140
#define TOOL_H 40
#define STATUS_H 22
#define CELL_W 92
#define CELL_H 86
#define ROW_H 24

enum {
    C_NEWDIR = 1, C_NEWFILE, C_OPEN, C_RENAME, C_DELETE, C_CLOSE,
    C_CUT = 10, C_COPY, C_PASTE,
    C_ICONS = 20, C_LIST, C_REFRESH, C_UP,
};
enum { D_NEWDIR = 1, D_NEWFILE, D_RENAME, D_DELETE };

struct files {
    char path[VFS_PATH_MAX];
    char target[VFS_PATH_MAX];              /* the item a dialog is about */
    struct dirent ent[MAXENT];
    int nent, sel, top;
    bool list_view;
};

static const struct { const char *label, *path; } places[] = {
    { "Macintosh HD", "/" }, { "tmp", "/tmp" }, { "mnt", "/mnt" }, { "dev", "/dev" },
};

static const struct gui_menu_item file_items[] = {
    { "New Folder", C_NEWDIR }, { "New File", C_NEWFILE }, { "-", 0 },
    { "Open", C_OPEN }, { "Rename", C_RENAME }, { "Delete", C_DELETE }, { "-", 0 }, { "Close Window", C_CLOSE },
};
static const struct gui_menu_item edit_items[] = {
    { "Cut", C_CUT }, { "Copy", C_COPY }, { "Paste", C_PASTE },
};
static const struct gui_menu_item view_items[] = {
    { "as Icons", C_ICONS }, { "as List", C_LIST }, { "-", 0 }, { "Go Up", C_UP }, { "Refresh", C_REFRESH },
};
static const struct gui_menu menus[] = {
    { "File", file_items, (int)ARRAY_SIZE(file_items) },
    { "Edit", edit_items, (int)ARRAY_SIZE(edit_items) },
    { "View", view_items, (int)ARRAY_SIZE(view_items) },
};

/* ---- folder contents ---- */

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
        if (strcmp(de.name, ".") && strcmp(de.name, ".."))
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

static void go_up(struct files *f)
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

static void item_path(const struct files *f, int i, char *out, size_t n)
{
    fo_join(out, n, f->path, f->ent[i].name);
}

static void reselect(struct files *f, const char *name)
{
    load(f);
    for (int i = 0; i < f->nent; i++)
        if (!strcmp(f->ent[i].name, name))
            f->sel = i;
}

static void fail(struct gui_window *w, const char *what, int rc)
{
    char msg[96];

    ksnprintf(msg, sizeof msg, "%s (error %d)", what, -rc);
    gui_alert(w, "Files", msg);
}

/* ---- layout ---- */

static int cols(const struct gui_window *w, const struct files *f)
{
    return f->list_view ? 1 : MAX(1, (w->cw - SIDE_W - 16) / CELL_W);
}

static int visible_rows(const struct gui_window *w, const struct files *f)
{
    int avail = w->ch - TOOL_H - STATUS_H;

    return MAX(1, avail / (f->list_view ? ROW_H : CELL_H));
}

static void ensure_visible(const struct gui_window *w, struct files *f)
{
    int c = cols(w, f), r = visible_rows(w, f);

    if (f->sel < 0)
        return;
    int row = f->sel / c;
    if (row < f->top)
        f->top = row;
    if (row >= f->top + r)
        f->top = row - r + 1;
}

static int item_at(const struct gui_window *w, const struct files *f, int x, int y)
{
    int c = cols(w, f);

    if (x < SIDE_W + 1 || y < TOOL_H || y >= w->ch - STATUS_H)
        return -1;
    int row, col;
    if (f->list_view) {
        row = (y - TOOL_H - 4) / ROW_H;
        col = 0;
        if (y < TOOL_H + 4)
            return -1;
    } else {
        row = (y - TOOL_H - 6) / CELL_H;
        col = (x - SIDE_W - 8) / CELL_W;
        if (col >= c || x < SIDE_W + 8 || y < TOOL_H + 6)
            return -1;
    }
    int i = (f->top + row) * c + col;
    return i >= 0 && i < f->nent ? i : -1;
}

/* ---- actions ---- */

static void activate(struct gui_window *w, struct files *f, int i)
{
    char p[VFS_PATH_MAX];

    if (i < 0 || i >= f->nent)
        return;
    item_path(f, i, p, sizeof p);
    if (f->ent[i].type == VFS_DIR)
        go(f, p);
    else if (f->ent[i].type == VFS_FILE)
        notepad_open_file(p);
    gui_redraw();
    (void)w;
}

static void do_copy_cut(struct files *f, bool cut)
{
    char p[VFS_PATH_MAX];

    if (f->sel < 0)
        return;
    item_path(f, f->sel, p, sizeof p);
    gui_clip_set(cut ? GUI_CLIP_FILES_CUT : GUI_CLIP_FILES_COPY, p);
}

static void do_paste(struct gui_window *w, struct files *f)
{
    int kind = gui_clip_kind();
    const char *src = gui_clip_get();
    char dst[VFS_PATH_MAX];
    int rc;

    if ((kind != GUI_CLIP_FILES_COPY && kind != GUI_CLIP_FILES_CUT) || !src[0])
        return;
    if (!fo_exists(src)) {
        gui_alert(w, "Files", "The copied item no longer exists.");
        return;
    }
    fo_join(dst, sizeof dst, f->path, fo_basename(src));
    if (kind == GUI_CLIP_FILES_CUT) {
        char srcdir[VFS_PATH_MAX];
        strlcpy(srcdir, src, sizeof srcdir);
        char *sl = strrchr(srcdir, '/');
        if (sl == srcdir) sl[1] = '\0'; else if (sl) *sl = '\0';
        if (!strcmp(srcdir, f->path))
            return;                                 /* moving into the same folder: nothing to do */
        if (fo_exists(dst))
            fo_copy_name(f->path, fo_basename(src), dst, sizeof dst);
        rc = fo_move(src, dst);
        if (rc == 0)
            gui_clip_set(GUI_CLIP_TEXT, "");
    } else {
        if (fo_exists(dst))
            fo_copy_name(f->path, fo_basename(src), dst, sizeof dst);
        rc = fo_copy(src, dst);
    }
    if (rc < 0)
        fail(w, kind == GUI_CLIP_FILES_CUT ? "Could not move the item" : "Could not copy the item", rc);
    reselect(f, fo_basename(dst));
}

static void ask_delete(struct gui_window *w, struct files *f)
{
    char msg[96];

    if (f->sel < 0)
        return;
    item_path(f, f->sel, f->target, sizeof f->target);
    ksnprintf(msg, sizeof msg, "Delete \"%s\"? This cannot be undone.", f->ent[f->sel].name);
    gui_confirm(w, "Delete", msg, D_DELETE);
}

static void ask_rename(struct gui_window *w, struct files *f)
{
    if (f->sel < 0)
        return;
    item_path(f, f->sel, f->target, sizeof f->target);
    gui_prompt(w, "Rename", f->ent[f->sel].name, D_RENAME);
}

static void ask_new(struct gui_window *w, bool dir)
{
    gui_prompt(w, dir ? "New Folder" : "New File", dir ? "untitled folder" : "untitled.txt",
               dir ? D_NEWDIR : D_NEWFILE);
}

static void files_dialog(struct gui_window *w, int id, bool ok, const char *text)
{
    struct files *f = w->priv;
    char p[VFS_PATH_MAX];
    int rc = 0;

    if (!ok)
        return;
    switch (id) {
    case D_NEWDIR:
    case D_NEWFILE:
        if (!text[0] || strchr(text, '/')) {
            gui_alert(w, "Files", "Please type a name without \"/\".");
            return;
        }
        fo_join(p, sizeof p, f->path, text);
        rc = id == D_NEWDIR ? fo_new_dir(p) : fo_new_file(p);
        if (rc < 0)
            fail(w, rc == -EEXIST ? "That name is already used" : "Could not create it", rc);
        reselect(f, text);
        break;
    case D_RENAME:
        rc = fo_rename(f->target, text);
        if (rc < 0)
            fail(w, rc == -EEXIST ? "That name is already used" : "Could not rename", rc);
        reselect(f, rc == 0 ? text : fo_basename(f->target));
        break;
    case D_DELETE: {
        int keep = f->sel;
        rc = fo_remove(f->target);
        if (rc < 0)
            fail(w, "Could not delete", rc);
        load(f);
        f->sel = f->nent ? MIN(keep, f->nent - 1) : -1;
        break;
    }
    }
    ensure_visible(w, f);
    gui_redraw();
}

static void files_command(struct gui_window *w, int id)
{
    struct files *f = w->priv;

    switch (id) {
    case C_NEWDIR:  ask_new(w, true); break;
    case C_NEWFILE: ask_new(w, false); break;
    case C_OPEN:    activate(w, f, f->sel); break;
    case C_RENAME:  ask_rename(w, f); break;
    case C_DELETE:  ask_delete(w, f); break;
    case C_CLOSE:   gui_close(w); break;
    case C_CUT:     do_copy_cut(f, true); break;
    case C_COPY:    do_copy_cut(f, false); break;
    case C_PASTE:   do_paste(w, f); break;
    case C_ICONS:   f->list_view = false; ensure_visible(w, f); break;
    case C_LIST:    f->list_view = true; ensure_visible(w, f); break;
    case C_REFRESH: load(f); break;
    case C_UP:      go_up(f); break;
    }
    gui_redraw();
}

/* ---- window callbacks ---- */

static void files_open(struct gui_window *w)
{
    struct files *f = kzalloc(sizeof *f);

    if (!f) {
        gui_close(w);
        return;
    }
    w->priv = f;
    go(f, "/");
}

static void files_close(struct gui_window *w) { kfree(w->priv); w->priv = NULL; }

static void short_name(const char *name, char *out, int max)
{
    int n = (int)strlen(name);

    if (n <= max) {
        strlcpy(out, name, (size_t)max + 1);
        return;
    }
    memcpy(out, name, (size_t)max - 2);
    out[max - 2] = '.';
    out[max - 1] = '.';
    out[max] = '\0';
}

static void files_draw(struct gui_window *w)
{
    struct files *f = w->priv;
    struct gfx_surface *s = w->surf;
    const struct gui_theme *t = gui_th;
    int c = cols(w, f), r = visible_rows(w, f);

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, t->win);
    gfx_fill_rect(s, 0, 0, SIDE_W, w->ch, t->side);
    gfx_vline(s, SIDE_W, 0, w->ch, t->line);
    gfx_text(s, 14, 12, "Favorites", t->dim, GFX_TRANSPARENT, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(places); i++)
        gui_sidebar_row(s, 0, 28 + (int)i * 26, SIDE_W, places[i].label, strcmp(f->path, places[i].path) == 0);

    /* toolbar */
    gfx_fill_rect(s, SIDE_W + 1, 0, w->cw - SIDE_W - 1, TOOL_H, t->title);
    gfx_hline(s, SIDE_W + 1, TOOL_H - 1, w->cw - SIDE_W - 1, t->line);
    gui_toolbar_button(s, SIDE_W + 10, 8, 30, 24, "<", strcmp(f->path, "/") != 0);
    gui_toolbar_button(s, SIDE_W + 46, 8, 78, 24, "New Folder", true);
    gui_toolbar_button(s, SIDE_W + 130, 8, 64, 24, "New File", true);
    gui_toolbar_button(s, SIDE_W + 200, 8, 40, 24, f->list_view ? "Icons" : "List", true);
    gfx_text(s, SIDE_W + 254, 16, f->path, t->dim, GFX_TRANSPARENT, 1);

    int visible = r * c;
    for (int k = 0; k < visible; k++) {
        int i = f->top * c + k;
        if (i >= f->nent)
            break;
        const struct dirent *e = &f->ent[i];
        bool sel = i == f->sel;
        char nm[40];
        if (f->list_view) {
            int y = TOOL_H + 4 + k * ROW_H;
            if (sel)
                gfx_fill_round_rect(s, SIDE_W + 6, y, w->cw - SIDE_W - 20, ROW_H - 2, 6, t->sel);
            gui_file_icon(s, SIDE_W + 14, y + 2, 18, (int)e->type, e->name);
            gfx_text(s, SIDE_W + 42, y + 8, e->name, sel ? t->sel_text : t->text, GFX_TRANSPARENT, 1);
            if (e->type != VFS_DIR) {
                char sz[16];
                ksnprintf(sz, sizeof sz, "%u", e->size);
                gfx_text(s, w->cw - 22 - gfx_text_width(sz, 1), y + 8, sz, sel ? t->sel_text : t->dim,
                         GFX_TRANSPARENT, 1);
            }
        } else {
            int col = k % c, row = k / c;
            int x = SIDE_W + 8 + col * CELL_W, y = TOOL_H + 6 + row * CELL_H;
            if (sel)
                gfx_fill_round_rect(s, x + 2, y, CELL_W - 4, CELL_H - 4, 8, t->sel);
            gui_file_icon(s, x + (CELL_W - 44) / 2, y + 6, 44, (int)e->type, e->name);
            short_name(e->name, nm, 10);
            gfx_text(s, x + (CELL_W - gfx_text_width(nm, 1)) / 2, y + 58, nm, sel ? t->sel_text : t->text,
                     GFX_TRANSPARENT, 1);
        }
    }
    if (!f->nent)
        gfx_text(s, SIDE_W + 20, TOOL_H + 20, "This folder is empty", t->dim, GFX_TRANSPARENT, 1);
    gui_scrollbar(s, w->cw - 8, TOOL_H + 4, w->ch - TOOL_H - STATUS_H - 8,
                  (f->nent + c - 1) / c, r, f->top);

    char st[96];
    gfx_hline(s, SIDE_W + 1, w->ch - STATUS_H, w->cw - SIDE_W - 1, t->line);
    if (f->sel >= 0 && f->sel < f->nent)
        ksnprintf(st, sizeof st, "%d items   \"%s\" selected", f->nent, f->ent[f->sel].name);
    else
        ksnprintf(st, sizeof st, "%d items", f->nent);
    gfx_text(s, SIDE_W + 12, w->ch - 15, st, t->dim, GFX_TRANSPARENT, 1);
}

static void popup_for(struct gui_window *w, struct files *f, int x, int y, bool on_item)
{
    bool can_paste = gui_clip_kind() != GUI_CLIP_TEXT;
    struct gui_menu_item it[8];
    int n = 0;

    if (on_item) {
        it[n++] = (struct gui_menu_item){ "Open", C_OPEN };
        it[n++] = (struct gui_menu_item){ "Rename", C_RENAME };
        it[n++] = (struct gui_menu_item){ "-", 0 };
        it[n++] = (struct gui_menu_item){ "Cut", C_CUT };
        it[n++] = (struct gui_menu_item){ "Copy", C_COPY };
        it[n++] = (struct gui_menu_item){ can_paste ? "Paste into this folder" : "~Paste into this folder", C_PASTE };
        it[n++] = (struct gui_menu_item){ "-", 0 };
        it[n++] = (struct gui_menu_item){ "Delete", C_DELETE };
    } else {
        it[n++] = (struct gui_menu_item){ "New Folder", C_NEWDIR };
        it[n++] = (struct gui_menu_item){ "New File", C_NEWFILE };
        it[n++] = (struct gui_menu_item){ "-", 0 };
        it[n++] = (struct gui_menu_item){ can_paste ? "Paste" : "~Paste", C_PASTE };
        it[n++] = (struct gui_menu_item){ f->list_view ? "View as Icons" : "View as List",
                                          f->list_view ? C_ICONS : C_LIST };
        it[n++] = (struct gui_menu_item){ "Refresh", C_REFRESH };
    }
    gui_popup(w, x, y, it, n);
}

static void files_click(struct gui_window *w, int x, int y, int buttons)
{
    struct files *f = w->priv;
    bool right = buttons & GUI_BTN_RIGHT;

    if (x < SIDE_W) {
        int i = (y - 28) / 26;
        if (!right && y >= 28 && i >= 0 && i < (int)ARRAY_SIZE(places))
            go(f, places[i].path);
    } else if (y < TOOL_H) {
        if (right)
            return;
        if (gui_hit(x, y, SIDE_W + 10, 8, 30, 24))
            go_up(f);
        else if (gui_hit(x, y, SIDE_W + 46, 8, 78, 24))
            ask_new(w, true);
        else if (gui_hit(x, y, SIDE_W + 130, 8, 64, 24))
            ask_new(w, false);
        else if (gui_hit(x, y, SIDE_W + 200, 8, 40, 24)) {
            f->list_view = !f->list_view;
            f->top = 0;
            ensure_visible(w, f);
        }
    } else {
        int i = item_at(w, f, x, y);
        if (i >= 0)
            f->sel = i;
        if (right) {
            popup_for(w, f, x, y, i >= 0);
        } else if ((buttons & GUI_BTN_DOUBLE) && i >= 0) {
            activate(w, f, i);
        }
    }
    gui_redraw();
}

static void files_key(struct gui_window *w, int key, int mods)
{
    struct files *f = w->priv;
    int c = cols(w, f), r = visible_rows(w, f);

    (void)mods;
    switch (key) {
    case KEY_LEFT:  if (!f->list_view) { if (f->sel > 0) f->sel--; } else go_up(f); break;
    case KEY_RIGHT: if (f->sel < f->nent - 1) f->sel++; break;
    case KEY_UP:    if (f->sel - c >= 0) f->sel -= c; break;
    case KEY_DOWN:  if (f->sel + c < f->nent) f->sel += c; else if (f->sel < f->nent - 1 && c > 1) f->sel = f->nent - 1; break;
    case KEY_PGUP:  f->sel = MAX(0, f->sel - r * c); break;
    case KEY_PGDN:  f->sel = MIN(f->nent - 1, f->sel + r * c); break;
    case KEY_HOME:  f->sel = f->nent ? 0 : -1; break;
    case KEY_END:   f->sel = f->nent - 1; break;
    case '\n':      activate(w, f, f->sel); break;
    case '\b':      go_up(f); break;
    case KEY_DELETE: ask_delete(w, f); break;
    case KEY_F(2):  ask_rename(w, f); break;
    case KEY_F(5):  load(f); break;
    case 3:         do_copy_cut(f, false); break;        /* Ctrl+C */
    case 24:        do_copy_cut(f, true); break;         /* Ctrl+X */
    case 22:        do_paste(w, f); break;               /* Ctrl+V */
    case 14:        ask_new(w, true); break;             /* Ctrl+N */
    }
    ensure_visible(w, f);
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
    .name = "Files", .title = "Files", .cw = 700, .ch = 440, .single = true, .dock = 20,
    .open = files_open, .close = files_close, .draw = files_draw, .key = files_key,
    .click = files_click, .icon = files_icon,
    .menus = menus, .nmenus = (int)ARRAY_SIZE(menus), .command = files_command, .dialog = files_dialog,
};
GUI_APP(files_app);
