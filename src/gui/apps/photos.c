/*
 * photos.c - "Photos": thumbnails of the pictures in a folder, click to view full size.
 * Reads BMP, PNG and JPEG (image.h). Keys in the viewer: Left / Right next picture,
 * Esc (or Backspace) back to the grid. File > Set as Wallpaper uses the current picture.
 */
#include "guiapi.h"
#include "image.h"
#include "sys.h"
#include "vfs.h"
#include "heap.h"
#include "input.h"
#include "kprintf.h"
#include "kstring.h"

#define MAXPIC 60
#define TW 112
#define TH 92
#define CELLW 124
#define CELLH 118
#define HEAD 34

struct pic { char name[40]; struct gfx_surface *thumb; bool tried; const char *err; };
struct photos {
    char dir[VFS_PATH_MAX];
    struct pic pic[MAXPIC];
    int n, top, view;                   /* view = -1 grid, else index being shown */
    struct gfx_surface *big;            /* picture in the viewer (already fitted) */
    const char *err;
};

enum { C_OPENDIR = 1, C_WALL, C_CLOSE, C_NEXT, C_PREV, C_GRID };
enum { D_DIR = 1 };

static const struct gui_menu_item file_items[] = {
    { "Open Folder...", C_OPENDIR }, { "Set as Wallpaper", C_WALL }, { "-", 0 }, { "Close Window", C_CLOSE } };
static const struct gui_menu_item view_items[] = {
    { "Next Picture", C_NEXT }, { "Previous Picture", C_PREV }, { "All Pictures", C_GRID } };
static const struct gui_menu menus[] = { { "File", file_items, 4 }, { "View", view_items, 3 } };

static void free_thumbs(struct photos *p)
{
    for (int i = 0; i < p->n; i++)
        gfx_surface_free(p->pic[i].thumb);
    gfx_surface_free(p->big);
    p->big = NULL;
    p->n = 0;
}

static void scan(struct photos *p, const char *dir)
{
    struct dirent e;
    free_thumbs(p);
    strlcpy(p->dir, dir, sizeof p->dir);
    p->top = 0;
    p->view = -1;
    for (uint32_t i = 0; vfs_readdir(dir, i, &e) == 0 && p->n < MAXPIC; i++)
        if (e.type == VFS_FILE && image_is_picture(e.name)) {
            memset(&p->pic[p->n], 0, sizeof p->pic[p->n]);
            strlcpy(p->pic[p->n].name, e.name, sizeof p->pic[p->n].name);
            p->n++;
        }
}

static void photos_open(struct gui_window *w)
{
    struct photos *p = kzalloc(sizeof *p);
    char d[VFS_PATH_MAX];
    w->priv = p;
    if (!p)
        return;
    ksnprintf(d, sizeof d, "%s/Pictures", users_current() ? users_home() : "/");
    scan(p, d);
}

static void photos_close(struct gui_window *w)
{
    struct photos *p = w->priv;
    if (p) {
        free_thumbs(p);
        kfree(p);
    }
}

static void path_of(struct photos *p, int i, char *out, size_t n)
{
    ksnprintf(out, n, "%s/%s", strcmp(p->dir, "/") ? p->dir : "", p->pic[i].name);
}

static void show(struct gui_window *w, struct photos *p, int i)
{
    char path[VFS_PATH_MAX];
    struct gfx_surface *raw;

    gfx_surface_free(p->big);
    p->big = NULL;
    p->err = NULL;
    p->view = i;
    path_of(p, i, path, sizeof path);
    raw = image_load(path, gui_th->win, &p->err);
    if (raw) {
        p->big = image_fit(raw, w->cw - 20, w->ch - HEAD - 12);
        gfx_surface_free(raw);
        if (!p->big)
            p->err = "out of memory";
    }
    gui_set_title(w, p->pic[i].name);
}

/* open a picture from other apps (Files) */
void photos_open_file(const char *path)
{
    struct gui_window *w = gui_open_named("Photos");
    struct photos *p;
    char dir[VFS_PATH_MAX];
    const char *slash = strrchr(path, '/');

    if (!w || !(p = w->priv))
        return;
    size_t dl = slash ? (size_t)(slash - path) : 0;
    memcpy(dir, path, dl);
    dir[dl] = 0;
    scan(p, dl ? dir : "/");
    for (int i = 0; i < p->n; i++)
        if (!strcmp(p->pic[i].name, slash ? slash + 1 : path)) {
            show(w, p, i);
            break;
        }
    gui_redraw();
}

static void photos_draw(struct gui_window *w)
{
    struct photos *p = w->priv;
    struct gfx_surface *s = w->surf;
    const struct gui_theme *t = gui_th;
    char line[96];

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, t->win);
    if (!p)
        return;
    gfx_fill_rect(s, 0, 0, w->cw, HEAD, t->title);
    gfx_hline(s, 0, HEAD - 1, w->cw, t->line);
    if (p->view >= 0) {
        gui_toolbar_button(s, 8, 5, 70, 24, "< All", true);
        gui_toolbar_button(s, w->cw - 82, 5, 34, 24, "<", p->view > 0);
        gui_toolbar_button(s, w->cw - 42, 5, 34, 24, ">", p->view + 1 < p->n);
        gfx_text(s, 92, 13, p->pic[p->view].name, t->text, GFX_TRANSPARENT, 1);
        if (p->big)
            gfx_blit(s, (w->cw - p->big->w) / 2, HEAD + (w->ch - HEAD - p->big->h) / 2, p->big, 0, 0, p->big->w, p->big->h);
        else
            gui_label_center(s, 0, w->ch / 2, w->cw, p->err ? p->err : "Loading...", t->dim);
        return;
    }
    ksnprintf(line, sizeof line, "%s   (%d pictures)", p->dir, p->n);
    gfx_text(s, 12, 13, line, t->dim, GFX_TRANSPARENT, 1);
    int cols = MAX(1, w->cw / CELLW);
    if (!p->n) {
        gui_label_center(s, 0, w->ch / 2 - 20, w->cw, "No pictures in this folder", t->dim);
        gui_label_center(s, 0, w->ch / 2, w->cw, "Put .bmp .png or .jpg files here, or use File > Open Folder", t->dim);
    }
    for (int i = p->top * cols; i < p->n; i++) {
        int r = i / cols - p->top, c = i % cols;
        int x = c * CELLW + 6, y = HEAD + 6 + r * CELLH;
        if (y + CELLH > w->ch + 4)
            break;
        gfx_fill_round_rect(s, x, y, TW + 4, TH + 4, 6, gfx_blend(t->win, t->line, 120));
        if (p->pic[i].thumb)
            gfx_blit(s, x + 2 + (TW - p->pic[i].thumb->w) / 2, y + 2 + (TH - p->pic[i].thumb->h) / 2,
                     p->pic[i].thumb, 0, 0, p->pic[i].thumb->w, p->pic[i].thumb->h);
        else
            gui_label_center(s, x, y + TH / 2 - 4, TW + 4, p->pic[i].tried ? "?" : "...", t->dim);
        char nm[16];
        strlcpy(nm, p->pic[i].name, sizeof nm);
        gfx_text(s, x + 2, y + TH + 10, nm, t->text, GFX_TRANSPARENT, 1);
    }
}

/* one thumbnail per tick keeps the desktop responsive */
static void photos_tick(struct gui_window *w)
{
    struct photos *p = w->priv;
    if (!p || p->view >= 0)
        return;
    for (int i = 0; i < p->n; i++)
        if (!p->pic[i].tried) {
            char path[VFS_PATH_MAX];
            struct gfx_surface *raw;
            p->pic[i].tried = true;
            path_of(p, i, path, sizeof path);
            raw = image_load(path, gui_th->win, &p->pic[i].err);
            if (raw) {
                p->pic[i].thumb = image_fit(raw, TW, TH);
                gfx_surface_free(raw);
            }
            gui_redraw();
            return;
        }
}

static void step(struct gui_window *w, struct photos *p, int d)
{
    int i = p->view + d;
    if (p->view >= 0 && i >= 0 && i < p->n)
        show(w, p, i);
}

static void photos_command(struct gui_window *w, int id)
{
    struct photos *p = w->priv;
    char path[VFS_PATH_MAX];

    if (!p)
        return;
    switch (id) {
    case C_OPENDIR: gui_prompt(w, "Open folder (path)", p->dir, D_DIR); break;
    case C_WALL:
        if (p->view >= 0) {
            path_of(p, p->view, path, sizeof path);
            if (!gui_set_wallpaper(path))
                gui_alert(w, "Photos", "Could not use this picture as wallpaper.");
        } else
            gui_alert(w, "Photos", "Open a picture first, then choose Set as Wallpaper.");
        break;
    case C_CLOSE: gui_close(w); break;
    case C_NEXT: step(w, p, 1); break;
    case C_PREV: step(w, p, -1); break;
    case C_GRID:
        p->view = -1;
        gui_set_title(w, "Photos");
        break;
    }
    gui_redraw();
}

static void photos_dialog(struct gui_window *w, int id, bool ok, const char *text)
{
    struct photos *p = w->priv;
    struct vfs_stat st;
    if (ok && p && id == D_DIR) {
        if (vfs_stat(text, &st) == 0 && st.type == VFS_DIR)
            scan(p, text);
        else
            gui_alert(w, "Photos", "That folder does not exist.");
    }
    gui_redraw();
}

static void photos_click(struct gui_window *w, int x, int y, int b)
{
    struct photos *p = w->priv;
    (void)b;
    if (!p)
        return;
    if (p->view >= 0) {
        if (gui_hit(x, y, 8, 5, 70, 24)) photos_command(w, C_GRID);
        else if (gui_hit(x, y, w->cw - 82, 5, 34, 24)) step(w, p, -1);
        else if (gui_hit(x, y, w->cw - 42, 5, 34, 24)) step(w, p, 1);
    } else if (y >= HEAD) {
        int cols = MAX(1, w->cw / CELLW);
        int i = ((y - HEAD - 6) / CELLH + p->top) * cols + (x - 6) / CELLW;
        if (i >= 0 && i < p->n && (x - 6) / CELLW < cols)
            show(w, p, i);
    }
    gui_redraw();
}

static void photos_key(struct gui_window *w, int key, int mods)
{
    struct photos *p = w->priv;
    (void)mods;
    if (!p)
        return;
    if (p->view >= 0) {
        if (key == KEY_RIGHT || key == ' ') step(w, p, 1);
        else if (key == KEY_LEFT) step(w, p, -1);
        else if (key == 27 || key == '\b') photos_command(w, C_GRID);
    } else {
        int cols = MAX(1, w->cw / CELLW), rows = MAX(1, (w->ch - HEAD) / CELLH);
        if (key == KEY_DOWN && (p->top + rows) * cols < p->n) p->top++;
        else if (key == KEY_UP && p->top > 0) p->top--;
    }
    gui_redraw();
}

static void photos_icon(struct gfx_surface *s, int x, int y, int sz, uint32_t g, uint32_t tile)
{
#define P(f) ((sz * (f)) / 100)
    gfx_fill_round_rect(s, x + P(16), y + P(22), P(68), P(56), P(6), g);
    gfx_fill_rect(s, x + P(22), y + P(28), P(56), P(44), tile);
    gfx_fill_circle(s, x + P(38), y + P(40), P(7), g);
    for (int i = 0; i < P(24); i++)
        gfx_hline(s, x + P(34) + i, y + P(70) - i, P(24) - i + 2, g);
#undef P
}

static const struct gui_app photos_app = {
    .name = "Photos", .title = "Photos", .cw = 640, .ch = 440, .single = true, .dock = 33,
    .open = photos_open, .close = photos_close, .draw = photos_draw, .click = photos_click, .key = photos_key,
    .tick = photos_tick, .menus = menus, .nmenus = 2, .command = photos_command, .dialog = photos_dialog,
    .icon = photos_icon,
};
GUI_APP(photos_app);
