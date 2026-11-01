/*
 * guiapi.h - how to write an application for the MyOS desktop.
 *
 * The desktop (src/gui/gui.c) only manages windows, the menu bar, the dock and
 * input. Every application lives in its own file under src/gui/apps/ (or
 * src/user/ for yours) and describes itself with a `struct gui_app`:
 *
 *     static void hello_draw(struct gui_window *w)
 *     {
 *         gfx_fill_rect(w->surf, 0, 0, w->cw, w->ch, gui_th->win);
 *         gfx_text(w->surf, 20, 20, "Hello!", gui_th->text, GFX_TRANSPARENT, 2);
 *     }
 *     static const struct gui_app hello_app = {
 *         .name = "Hello", .title = "Hello", .cw = 300, .ch = 200,
 *         .dock = 90, .draw = hello_draw,
 *     };
 *     GUI_APP(hello_app);
 *
 * That is all: the app appears in the dock and in the Apple menu. See
 * src/user/hello_app.c for a complete example with keyboard and mouse.
 *
 * Keep GUI code (src/gui) and kernel code (everything else) separate: apps only
 * use gfx.h, this header, and the normal kernel APIs (vfs.h, sched.h ...).
 */
#ifndef GUIAPI_H
#define GUIAPI_H
#include "types.h"
#include "gfx.h"

struct gui_theme {
    uint32_t bg_top, bg_bot;        /* wallpaper gradient              */
    uint32_t win, side;             /* window and sidebar background   */
    uint32_t title, title_off;      /* title bar (active / inactive)   */
    uint32_t text, dim, line;       /* text, secondary text, separators */
    uint32_t menu;                  /* menu bar and menus              */
    uint32_t sel, sel_text;         /* selected row                    */
    uint32_t accent, accent_fg;     /* filled buttons                  */
    uint32_t tile, tile_edge, glyph;/* dock icon tile and drawing colour */
    uint32_t tl_mono[3];            /* window buttons when monochrome  */
};

struct gui_prefs {
    bool dark;                      /* dark theme                      */
    bool anim;                      /* animations                      */
    bool color_buttons;             /* red/yellow/green window buttons */
    int  accent;                    /* index into the accent colours (6 = graphite / monochrome) */
    int  pixfmt;                    /* 0 auto, 1 RGB, 2 BGR (display colour order) */
    int  ptr_speed;                 /* 0 slow, 1 normal, 2 fast        */
};

extern const struct gui_theme *gui_th;      /* colours of the current theme */
extern const struct gui_theme gui_theme_light, gui_theme_dark;
extern struct gui_prefs gui_prefs;

#define GUI_ACCENTS 7
extern const char *const gui_accent_name[GUI_ACCENTS];
uint32_t gui_accent_color(int index, bool dark);   /* swatch colour */
void gui_theme_apply(void);         /* rebuild gui_th from prefs (dark, accent) */
void gui_refresh_theme(void);       /* apply + repaint everything */

struct gui_window;

struct gui_app {
    const char *name;               /* dock label and menu entry       */
    const char *title;              /* window title (NULL = name)      */
    int cw, ch;                     /* client area size in pixels      */
    void (*size)(int *cw, int *ch); /* optional: choose the size at run time */
    bool single;                    /* only one window at a time       */
    int dock;                       /* dock position (low = left), <0 = not in the dock */
    bool dark_tile;                 /* black icon tile in both themes  */

    /* All callbacks are optional except draw. */
    void (*open)(struct gui_window *w);                       /* window created        */
    void (*close)(struct gui_window *w);                      /* window is going away  */
    void (*draw)(struct gui_window *w);                       /* paint w->surf         */
    void (*key)(struct gui_window *w, int key, int mods);     /* keyboard (focused)    */
    void (*click)(struct gui_window *w, int x, int y, int buttons);  /* x,y inside client */
    void (*tick)(struct gui_window *w);                       /* about 10 times a second */
    /* Draw the icon glyph into a (sz x sz) square at x,y. `fg` is the glyph colour. */
    void (*icon)(struct gfx_surface *s, int x, int y, int sz, uint32_t fg, uint32_t bg);
};

struct gui_window {
    bool used;
    const struct gui_app *app;
    int x, y;                       /* top-left on screen              */
    int cw, ch;                     /* client size                     */
    char title[40];
    struct gfx_surface *surf;       /* draw here                       */
    uint32_t born;                  /* tick when opened                */
    bool want_close;
    void *priv;                     /* the app's own data              */
};

#define GUI_APP(var) \
    static const struct gui_app *const __gui_app_##var \
        __attribute__((section(".gui_apps"), used)) = &(var)

/* ---- services the desktop offers to applications ---- */
int   gui_screen_w(void);
int   gui_screen_h(void);
struct gui_window *gui_open(const struct gui_app *app);
struct gui_window *gui_open_named(const char *name);
void  gui_close(struct gui_window *w);          /* safe to call from callbacks */
void  gui_redraw(void);                         /* ask for a new frame         */
void  gui_set_title(struct gui_window *w, const char *title);
void  gui_set_dark(bool dark);
void  gui_quit(void);                           /* leave the desktop           */
int   gui_app_count(void);
const struct gui_app *gui_app_at(int i);

/* ---- small drawing helpers (src/gui/widgets.c) ---- */
void gui_button(struct gfx_surface *s, int x, int y, int w, int h, const char *label, bool on);
void gui_segment(struct gfx_surface *s, int x, int y, int w, const char **labels, int n, int active);
void gui_sidebar_row(struct gfx_surface *s, int x, int y, int w, const char *label, bool sel);
void gui_label_center(struct gfx_surface *s, int x, int y, int w, const char *text, uint32_t col);

/* ---- applications that other apps call ---- */
void notepad_open_file(const char *path);       /* src/gui/apps/notepad.c */

#endif
