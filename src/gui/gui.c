/*
 * gui.c - the desktop: window manager, menu bar, dock, Spotlight search,
 * pointer and input handling. It knows nothing about individual applications:
 * those register themselves with GUI_APP() (see guiapi.h and src/gui/apps/).
 *
 * Look: white (light theme) or black (dark theme), rounded windows with
 * window buttons on the left, a translucent menu bar with the MyOS menu,
 * and a dock with zoom and bounce animations.
 */
#include "gui.h"
#include "guiapi.h"
#include "gfx.h"
#include "fbcon.h"
#include "input.h"
#include "console.h"
#include "tgrid.h"
#include "timer.h"
#include "sched.h"
#include "hw.h"
#include "kernel.h"
#include "kprintf.h"
#include "kstring.h"
#include "serial.h"
#include "multiboot.h"
#include "io.h"
#include "kernel.h"

#define TITLE_H    30
#define MENUBAR_H  28
#define DOCK_RESERVE 90         /* screen space kept free for the dock */
#define MAX_WINDOWS 12
#define MAX_APPS    24

extern const struct gui_app *const __start_gui_apps[];
extern const struct gui_app *const __stop_gui_apps[];

static const struct gui_app *apps[MAX_APPS];
static int napps;

static struct gui_window wins[MAX_WINDOWS];
static int zorder[MAX_WINDOWS];
static int nz;

static struct gfx_surface *wallpaper;
static int sw, sh;
static int mx, my;
static uint8_t mbuttons;
static bool running, animating, need_redraw;
static bool ptr_mode;

/* menus */
enum { M_NONE, M_APPLE };
static int menu_open;
struct mitem { char label[32]; int code; };      /* code: >=0 open app, <0 special, -9 separator */
static struct mitem mitems[MAX_APPS + 8];
static int nmitems;
#define MI_QUIT -1
#define MI_RESTART -2
#define MI_SHUTDOWN -3
#define MI_SEP -9
#define MENU_W 230
#define MENU_ITEM_H 26

/* spotlight */
static bool spot_open;
static char spot_q[32];
static int spot_n, spot_sel;
static int spot_hits[MAX_APPS];

/* ------------------------------------------------------------------ */
/* Applications                                                        */
/* ------------------------------------------------------------------ */

static void collect_apps(void)
{
    napps = 0;
    for (const struct gui_app *const *p = __start_gui_apps; p < __stop_gui_apps && napps < MAX_APPS; p++)
        apps[napps++] = *p;
    for (int i = 1; i < napps; i++) {                   /* sort by dock position */
        const struct gui_app *k = apps[i];
        int j = i - 1;
        while (j >= 0 && apps[j]->dock > k->dock) {
            apps[j + 1] = apps[j];
            j--;
        }
        apps[j + 1] = k;
    }
}

int gui_app_count(void) { return napps; }
const struct gui_app *gui_app_at(int i) { return i >= 0 && i < napps ? apps[i] : NULL; }
int gui_screen_w(void) { return sw ? sw : gfx_width(); }
int gui_screen_h(void) { return sh ? sh : gfx_height(); }
void gui_redraw(void) { need_redraw = true; }
void gui_quit(void) { running = false; }

static int win_index(const struct gui_window *w) { return (int)(w - wins); }
static int outer_w(const struct gui_window *w) { return w->cw + 2; }
static int outer_h(const struct gui_window *w) { return w->ch + TITLE_H + 1; }

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

static struct gui_window *focused(void) { return nz ? &wins[zorder[nz - 1]] : NULL; }

void gui_set_title(struct gui_window *w, const char *t) { strlcpy(w->title, t, sizeof w->title); }

struct gui_window *gui_open(const struct gui_app *app)
{
    if (app->single)
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (wins[i].used && wins[i].app == app && !wins[i].want_close) {
                bring_to_front(i);
                return &wins[i];
            }
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!wins[i].used) {
            slot = i;
            break;
        }
    if (slot < 0)
        return NULL;
    int cw = app->cw, ch = app->ch;
    if (app->size)
        app->size(&cw, &ch);
    cw = MIN(cw, sw - 20);
    ch = MIN(ch, sh - DOCK_RESERVE - MENUBAR_H - TITLE_H - 16);
    struct gui_window *w = &wins[slot];
    memset(w, 0, sizeof *w);
    w->surf = gfx_surface_create(cw, ch);
    if (!w->surf)
        return NULL;
    w->used = true;
    w->app = app;
    w->cw = cw;
    w->ch = ch;
    w->born = timer_ticks();
    strlcpy(w->title, app->title ? app->title : app->name, sizeof w->title);
    int off = (nz % 6) * 26;
    w->x = MAX(10, (sw - outer_w(w)) / 2 - 60 + off);
    w->y = MAX(MENUBAR_H + 8, (sh - DOCK_RESERVE - outer_h(w)) / 2 + off - 10);
    zorder[nz++] = slot;
    if (app->open)
        app->open(w);
    gui_redraw();
    return w;
}

struct gui_window *gui_open_named(const char *name)
{
    for (int i = 0; i < napps; i++)
        if (strcmp(apps[i]->name, name) == 0)
            return gui_open(apps[i]);
    return NULL;
}

void gui_close(struct gui_window *w)
{
    w->want_close = true;
    gui_redraw();
}

static void destroy_window(struct gui_window *w)
{
    int idx = win_index(w);

    if (w->app->close)
        w->app->close(w);
    gfx_surface_free(w->surf);
    w->surf = NULL;
    w->used = false;
    int j = 0;
    for (int i = 0; i < nz; i++)
        if (zorder[i] != idx)
            zorder[j++] = zorder[i];
    nz = j;
}

static void reap_windows(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used && wins[i].want_close)
            destroy_window(&wins[i]);
}

/* ------------------------------------------------------------------ */
/* Theme and wallpaper                                                 */
/* ------------------------------------------------------------------ */

static void build_wallpaper(void)
{
    gfx_surface_free(wallpaper);
    wallpaper = gfx_surface_create(sw, sh);
    if (!wallpaper)
        return;
    gfx_gradient_v(wallpaper, 0, 0, sw, sh, gui_th->bg_top, gui_th->bg_bot);
    uint32_t c1 = gfx_blend(gui_th->bg_top, gui_th->bg_bot, 80), c2 = gfx_blend(gui_th->bg_top, gui_th->bg_bot, 150);
    gfx_fill_circle(wallpaper, sw * 78 / 100, sh * 30 / 100, sh / 3, c1);
    gfx_fill_circle(wallpaper, sw * 22 / 100, sh * 80 / 100, sh / 4, c2);
}

void gui_set_dark(bool dark)
{
    gui_prefs.dark = dark;
    gui_th = dark ? &gui_theme_dark : &gui_theme_light;
    tg_light_mode = !dark;
    build_wallpaper();
    gui_redraw();
}

/* ------------------------------------------------------------------ */
/* Icons and the dock                                                  */
/* ------------------------------------------------------------------ */

#define DOCK_ICON 52
#define DOCK_GAP 14
#define MAX_DOCK 16

static int dock_idx[MAX_DOCK];                  /* indexes into apps[] */
static int ndock;
static int dock_sc[MAX_DOCK];
static uint32_t dock_bounce[MAX_DOCK];

static void build_dock(void)
{
    ndock = 0;
    for (int i = 0; i < napps && ndock < MAX_DOCK; i++)
        if (apps[i]->dock >= 0)
            dock_idx[ndock++] = i;
}

static void dock_rect(int *x, int *y, int *w, int *h)
{
    *w = ndock * DOCK_ICON + (ndock - 1) * DOCK_GAP + 32;
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

static bool app_running(const struct gui_app *a)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used && wins[i].app == a && !wins[i].want_close)
            return true;
    return false;
}

static void launch(const struct gui_app *a)
{
    if (gui_prefs.anim)
        for (int i = 0; i < ndock; i++)
            if (apps[dock_idx[i]] == a)
                dock_bounce[i] = timer_ticks() | 1;
    gui_open(a);
}

static void draw_app_icon(struct gfx_surface *s, const struct gui_app *a, int x, int y, int sz)
{
    uint32_t tile = gui_th->tile, fg = gui_th->glyph, edge = gui_th->tile_edge;

    if (a->dark_tile) {
        tile = GFX_RGB(16, 16, 18);
        fg = GFX_RGB(248, 248, 250);
        edge = gui_prefs.dark ? GFX_RGB(110, 110, 118) : tile;
    }
    gfx_fill_round_rect(s, x - 1, y - 1, sz + 2, sz + 2, sz * 24 / 100, edge);
    gfx_fill_round_rect(s, x, y, sz, sz, sz * 23 / 100, tile);
    if (a->icon)
        a->icon(s, x, y, sz, fg, tile);
    else
        gfx_char(s, x + sz / 2 - 8, y + sz / 2 - 8, a->name[0], fg, GFX_TRANSPARENT, 2);
}

static void draw_dock(struct gfx_surface *s)
{
    int x, y, w, h;
    uint32_t now = timer_ticks();

    if (!ndock)
        return;
    dock_rect(&x, &y, &w, &h);
    gfx_fill_round_rect(s, x - 1, y - 1, w + 2, h + 2, 19, gui_th->line);
    gfx_fill_round_rect(s, x, y, w, h, 18, gfx_blend(gfx_get_pixel(s, x + w / 2, y + 2), gui_th->menu, 215));
    for (int i = 0; i < ndock; i++) {
        const struct gui_app *a = apps[dock_idx[i]];
        int ix = dock_icon_x(i);
        bool hot = mx >= ix - 4 && mx < ix + DOCK_ICON + 4 && my >= y - 6 && my < y + h;
        int target = hot ? 8 : 0;
        if (!gui_prefs.anim)
            dock_sc[i] = target;
        else if (dock_sc[i] != target) {
            dock_sc[i] += dock_sc[i] < target ? 2 : -2;
            animating = true;
        }
        int sz = DOCK_ICON + dock_sc[i];
        int dx = ix - (sz - DOCK_ICON) / 2, dy = y + 7 - dock_sc[i];
        if (dock_bounce[i]) {
            int dt = (int)((now - dock_bounce[i]) / 2);
            if (dt < 16) {
                dy -= dt < 8 ? dt * 2 : (16 - dt) * 2;
                animating = true;
            } else
                dock_bounce[i] = 0;
        }
        draw_app_icon(s, a, dx, dy, sz);
        if (app_running(a))
            gfx_fill_circle(s, ix + DOCK_ICON / 2, y + h - 6, 2, gui_th->glyph);
        if (hot && dock_sc[i] > 3) {
            const char *label = a->name;
            int lw = gfx_text_width(label, 1) + 16;
            int lx = ix + DOCK_ICON / 2 - lw / 2, ly = y - 34;
            gfx_fill_round_rect(s, lx, ly, lw, 20, 6, gui_th->accent);
            gfx_text(s, lx + 8, ly + 6, label, gui_th->accent_fg, GFX_TRANSPARENT, 1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Windows                                                             */
/* ------------------------------------------------------------------ */

static void draw_window(struct gfx_surface *s, struct gui_window *w, bool active)
{
    int ow = outer_w(w), oh = outer_h(w), savey = w->y;

    if (gui_prefs.anim) {                               /* open animation: slide up, 200 ms */
        int t = (int)(timer_ticks() - w->born);
        if (t < 20) {
            int rem = 20 - t;
            w->y += 30 * rem * rem / 400;
            animating = true;
        }
    }
    for (int k = 5; k >= 0; k--)
        gfx_fill_round_rect(s, w->x - k * 2, w->y + 6 - k, ow + k * 4, oh + k * 3, 14,
                            gfx_blend(gfx_get_pixel(s, w->x + ow / 2, MIN(sh - 1, w->y + oh + 4)), GFX_BLACK, 9));
    gfx_fill_round_rect(s, w->x - 1, w->y - 1, ow + 2, oh + 2, 11, gui_th->line);
    gfx_fill_round_rect(s, w->x, w->y, ow, oh, 10, gui_th->win);
    gfx_fill_round_rect(s, w->x, w->y, ow, TITLE_H + 12, 10, active ? gui_th->title : gui_th->title_off);
    gfx_fill_rect(s, w->x, w->y + TITLE_H, ow, 12, gui_th->win);
    gfx_hline(s, w->x, w->y + TITLE_H - 1, ow, gui_th->line);

    static const uint32_t colors[3] = { GFX_RGB(255, 95, 87), GFX_RGB(254, 188, 46), GFX_RGB(40, 200, 64) };
    for (int i = 0; i < 3; i++) {
        uint32_t c = !active ? gui_th->line : (gui_prefs.color_buttons ? colors[i] : gui_th->tl_mono[i]);
        gfx_fill_circle(s, w->x + 18 + i * 20, w->y + 15, 6, c);
    }
    int tw = gfx_text_width(w->title, 1);
    gfx_text(s, w->x + (ow - tw) / 2, w->y + 11, w->title, active ? gui_th->text : gui_th->dim, GFX_TRANSPARENT, 1);

    if (w->app->draw)
        w->app->draw(w);
    gfx_blit(s, w->x + 1, w->y + TITLE_H, w->surf, 0, 0, w->cw, w->ch);
    w->y = savey;
}

/* ------------------------------------------------------------------ */
/* Menu bar, MyOS menu, Spotlight                                      */
/* ------------------------------------------------------------------ */

static void build_menu(void)
{
    nmitems = 0;
#define ADD(l, c) do { strlcpy(mitems[nmitems].label, (l), 32); mitems[nmitems++].code = (c); } while (0)
    for (int i = 0; i < napps; i++)
        if (strcmp(apps[i]->name, "About") == 0)
            ADD("About MyOS", i);
    ADD("", MI_SEP);
    for (int i = 0; i < napps; i++)
        if (strcmp(apps[i]->name, "Settings") == 0)
            ADD("System Settings...", i);
    ADD("", MI_SEP);
    for (int i = 0; i < napps && nmitems < MAX_APPS + 4; i++)
        if (strcmp(apps[i]->name, "About") && strcmp(apps[i]->name, "Settings"))
            ADD(apps[i]->name, i);
    ADD("", MI_SEP);
    ADD("Exit to Text Console", MI_QUIT);
    ADD("Restart...", MI_RESTART);
    ADD("Shut Down...", MI_SHUTDOWN);
#undef ADD
}

static void menu_rect(int *x, int *y, int *w, int *h)
{
    int hh = 10;

    for (int i = 0; i < nmitems; i++)
        hh += mitems[i].code == MI_SEP ? 9 : MENU_ITEM_H;
    *x = 8; *y = MENUBAR_H + 2; *w = MENU_W; *h = hh;
}

static void draw_menu(struct gfx_surface *s)
{
    int x, y, w, h;

    menu_rect(&x, &y, &w, &h);
    gfx_fill_round_rect(s, x + 2, y + 4, w, h, 10, gfx_blend(gfx_get_pixel(s, x + 4, y + h + 6), GFX_BLACK, 40));
    gfx_fill_round_rect(s, x - 1, y - 1, w + 2, h + 2, 11, gui_th->line);
    gfx_fill_round_rect(s, x, y, w, h, 10, gui_th->menu);
    int iy = y + 5;
    for (int i = 0; i < nmitems; i++) {
        if (mitems[i].code == MI_SEP) {
            gfx_hline(s, x + 12, iy + 4, w - 24, gui_th->line);
            iy += 9;
            continue;
        }
        bool hot = mx >= x && mx < x + w && my >= iy && my < iy + MENU_ITEM_H;
        if (hot)
            gfx_fill_round_rect(s, x + 5, iy, w - 10, MENU_ITEM_H - 2, 6, gui_th->accent);
        gfx_text(s, x + 16, iy + 9, mitems[i].label, hot ? gui_th->accent_fg : gui_th->text, GFX_TRANSPARENT, 1);
        iy += MENU_ITEM_H;
    }
}

static int menu_hit(int px, int py)
{
    int x, y, w, h;

    menu_rect(&x, &y, &w, &h);
    if (px < x || px >= x + w || py < y || py >= y + h)
        return -2;
    int iy = y + 5;
    for (int i = 0; i < nmitems; i++) {
        int ih = mitems[i].code == MI_SEP ? 9 : MENU_ITEM_H;
        if (py >= iy && py < iy + ih)
            return mitems[i].code == MI_SEP ? -2 : i;
        iy += ih;
    }
    return -2;
}

static void menu_action(int i)
{
    int code = mitems[i].code;

    menu_open = M_NONE;
    if (code >= 0)
        launch(apps[code]);
    else if (code == MI_QUIT)
        gui_quit();
    else if (code == MI_SHUTDOWN)
        system_poweroff();
    else if (code == MI_RESTART)
        system_reboot();
}

static void draw_menubar(struct gfx_surface *s)
{
    gfx_fill_rect_alpha(s, 0, 0, sw, MENUBAR_H, gui_th->menu, 225);
    gfx_hline(s, 0, MENUBAR_H - 1, sw, gui_th->line);
    if (menu_open)
        gfx_fill_round_rect(s, 4, 3, 36, MENUBAR_H - 6, 6, gui_th->sel);
    gfx_draw_logo(s, 14, 6, 16);
    struct gui_window *f = focused();
    const char *name = f ? f->app->name : "Finder";
    gfx_text(s, 52, 10, name, gui_th->text, GFX_TRANSPARENT, 1);      /* bold: draw twice */
    gfx_text(s, 53, 10, name, gui_th->text, GFX_TRANSPARENT, 1);
    if (ptr_mode)
        gfx_text(s, sw / 2 - 150, 10, "POINTER MODE: arrows move, Enter clicks, F2/Esc exits",
                 gui_th->dim, GFX_TRANSPARENT, 1);
    uint32_t tm[6];
    char clock[24];
    rtc_read(tm);
    ksnprintf(clock, sizeof clock, "%02u:%02u", tm[3], tm[4]);
    gfx_text(s, sw - gfx_text_width(clock, 1) - 14, 10, clock, gui_th->text, GFX_TRANSPARENT, 1);
    /* magnifier = Spotlight */
    int cx = sw - gfx_text_width(clock, 1) - 44, cy = 12;
    gfx_fill_circle(s, cx, cy, 5, gui_th->text);
    gfx_fill_circle(s, cx, cy, 3, gui_th->menu);
    gfx_line(s, cx + 4, cy + 4, cx + 8, cy + 8, gui_th->text);
    gfx_line(s, cx + 5, cy + 4, cx + 9, cy + 8, gui_th->text);
}

static void spot_update(void)
{
    char q[32];
    int n = 0;

    strlcpy(q, spot_q, sizeof q);
    for (char *c = q; *c; c++)
        *c = (char)tolower(*c);
    for (int i = 0; i < napps; i++) {
        char name[32];
        strlcpy(name, apps[i]->name, sizeof name);
        for (char *c = name; *c; c++)
            *c = (char)tolower(*c);
        if (!q[0] || strstr(name, q))
            spot_hits[n++] = i;
    }
    spot_n = n;
    if (spot_sel >= n)
        spot_sel = MAX(0, n - 1);
}

static void spot_rect(int *x, int *y, int *w, int *h)
{
    *w = 520;
    *x = (sw - *w) / 2;
    *y = MENUBAR_H + 70;
    *h = 54 + (spot_n ? spot_n * 32 + 8 : 0);
}

static void draw_spotlight(struct gfx_surface *s)
{
    int x, y, w, h;

    spot_rect(&x, &y, &w, &h);
    gfx_fill_round_rect(s, x + 3, y + 6, w, h, 16, gfx_blend(gfx_get_pixel(s, x + 10, y + h + 8), GFX_BLACK, 50));
    gfx_fill_round_rect(s, x - 1, y - 1, w + 2, h + 2, 17, gui_th->line);
    gfx_fill_round_rect(s, x, y, w, h, 16, gui_th->menu);
    gfx_fill_circle(s, x + 28, y + 26, 7, gui_th->dim);
    gfx_fill_circle(s, x + 28, y + 26, 5, gui_th->menu);
    gfx_line(s, x + 33, y + 31, x + 38, y + 36, gui_th->dim);
    if (spot_q[0])
        gfx_text(s, x + 54, y + 22, spot_q, gui_th->text, GFX_TRANSPARENT, 2);
    else
        gfx_text(s, x + 54, y + 22, "Spotlight Search", gui_th->dim, GFX_TRANSPARENT, 2);
    if (spot_n)
        gfx_hline(s, x + 12, y + 52, w - 24, gui_th->line);
    for (int i = 0; i < spot_n; i++) {
        int ry = y + 58 + i * 32;
        if (i == spot_sel)
            gfx_fill_round_rect(s, x + 8, ry, w - 16, 30, 8, gui_th->accent);
        draw_app_icon(s, apps[spot_hits[i]], x + 18, ry + 3, 24);
        gfx_text(s, x + 54, ry + 11, apps[spot_hits[i]]->name,
                 i == spot_sel ? gui_th->accent_fg : gui_th->text, GFX_TRANSPARENT, 1);
    }
}

static void spot_toggle(void)
{
    spot_open = !spot_open;
    spot_q[0] = '\0';
    spot_sel = 0;
    menu_open = M_NONE;
    spot_update();
}

static bool spot_key(int k)
{
    int len = (int)strlen(spot_q);

    if (k == 27) {
        spot_open = false;
    } else if (k == '\n') {
        if (spot_n)
            launch(apps[spot_hits[spot_sel]]);
        spot_open = false;
    } else if (k == KEY_DOWN) {
        if (spot_sel < spot_n - 1) spot_sel++;
    } else if (k == KEY_UP) {
        if (spot_sel > 0) spot_sel--;
    } else if (k == '\b' || k == 127) {
        if (len) spot_q[len - 1] = '\0';
    } else if (k >= 32 && k < 127 && len < 24) {
        spot_q[len] = (char)k;
        spot_q[len + 1] = '\0';
    }
    spot_update();
    return true;
}

/* ------------------------------------------------------------------ */
/* Cursor and frame                                                    */
/* ------------------------------------------------------------------ */

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

static void render(void)
{
    struct gfx_surface *s = gfx_screen();

    animating = false;
    if (wallpaper)
        gfx_blit(s, 0, 0, wallpaper, 0, 0, sw, sh);
    else
        gfx_fill_rect(s, 0, 0, sw, sh, gui_th->bg_top);
    for (int i = 0; i < nz; i++)
        draw_window(s, &wins[zorder[i]], i == nz - 1);
    draw_dock(s);
    draw_menubar(s);
    if (menu_open)
        draw_menu(s);
    if (spot_open)
        draw_spotlight(s);
    draw_cursor(s);
    gfx_flush_all();
}

/* ------------------------------------------------------------------ */
/* Splash                                                              */
/* ------------------------------------------------------------------ */

void gui_splash(void)
{
    if (!gfx_available())
        return;
    struct gfx_surface *s = gfx_screen();
    int w = gfx_width(), h = gfx_height();
    int size = MIN(w, h) / 5;
    int ly = h / 2 - size / 2 - 30;
    static const char *steps[] = {
        "Starting the memory manager...", "Mounting file systems...",
        "Loading input drivers...", "Starting the desktop...",
    };
    bool dark = gui_prefs.dark;
    uint32_t top = dark ? GFX_RGB(30, 30, 33) : GFX_RGB(255, 255, 255);
    uint32_t bot = dark ? GFX_RGB(0, 0, 0) : GFX_RGB(222, 224, 230);
    uint32_t fg = dark ? GFX_RGB(245, 245, 248) : GFX_RGB(24, 24, 26);

    for (int step = 0; step <= 4; step++) {
        gfx_gradient_v(s, 0, 0, w, h, top, bot);
        int sz = size * (step == 0 ? 80 : 100) / 100;
        gfx_draw_logo(s, (w - sz) / 2, ly + (size - sz) / 2, sz);
        gfx_text(s, (w - gfx_text_width("MyOS", 4)) / 2, ly + size + 22, "MyOS", fg, GFX_TRANSPARENT, 4);
        int bw = size * 2, bx = (w - bw) / 2, by = ly + size + 80;
        gfx_fill_round_rect(s, bx, by, bw, 6, 3, dark ? GFX_RGB(70, 70, 76) : GFX_RGB(214, 214, 220));
        if (step > 0)
            gfx_fill_round_rect(s, bx, by, bw * step / 4, 6, 3, fg);
        const char *msg = step < 4 ? steps[step] : "Welcome";
        gfx_text(s, (w - gfx_text_width(msg, 1)) / 2, by + 20, msg, GFX_RGB(130, 130, 138), GFX_TRANSPARENT, 1);
        gfx_flush_all();
        timer_sleep_ms(step < 4 ? 260 : 350);
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static int drag_idx = -1, drag_dx, drag_dy;

static int hit_window(int x, int y)
{
    for (int i = nz - 1; i >= 0; i--) {
        struct gui_window *w = &wins[zorder[i]];
        if (x >= w->x && x < w->x + outer_w(w) && y >= w->y && y < w->y + outer_h(w))
            return zorder[i];
    }
    return -1;
}

static void mouse_click(int x, int y)
{
    if (spot_open) {
        int sx, sy, swd, sht;
        spot_rect(&sx, &sy, &swd, &sht);
        if (x >= sx && x < sx + swd && y >= sy + 58 && y < sy + sht) {
            int i = (y - sy - 58) / 32;
            if (i >= 0 && i < spot_n)
                launch(apps[spot_hits[i]]);
        }
        spot_open = false;
        return;
    }
    if (menu_open) {
        int i = menu_hit(x, y);
        if (i >= 0) {
            menu_action(i);
            return;
        }
        menu_open = M_NONE;
        if (y >= MENUBAR_H)
            return;
    }
    if (y < MENUBAR_H) {
        if (x < 44)
            menu_open = menu_open ? M_NONE : M_APPLE;
        else if (x > sw - 90)
            spot_toggle();
        return;
    }
    if (ndock) {
        int dx, dy, dw, dh;
        dock_rect(&dx, &dy, &dw, &dh);
        if (x >= dx && x < dx + dw && y >= dy - 10 && y < dy + dh) {
            for (int i = 0; i < ndock; i++) {
                int ix = dock_icon_x(i);
                if (x >= ix - 4 && x < ix + DOCK_ICON + 4)
                    launch(apps[dock_idx[i]]);
            }
            return;
        }
    }
    int idx = hit_window(x, y);
    if (idx < 0)
        return;
    struct gui_window *w = &wins[idx];
    bring_to_front(idx);
    int lx = x - w->x, ly = y - w->y;
    if (ly < TITLE_H) {
        if (lx < 70) {
            if (lx < 28)
                gui_close(w);
            return;
        }
        drag_idx = idx;
        drag_dx = lx;
        drag_dy = ly;
        return;
    }
    if (w->app->click)
        w->app->click(w, lx - 1, ly - TITLE_H, 1);
}

static void handle_mouse(const struct input_event *ev)
{
    int ax = ev->dx, ay = ev->dy;

    if (ax > 6 || ax < -6) ax *= 2;
    if (ay > 6 || ay < -6) ay *= 2;
    static const int mult[3] = { 2, 3, 5 };
    int sp = MAX(0, MIN(2, gui_prefs.ptr_speed));
    if (ev->pad & 1) {                                  /* tablet: absolute position 0..32767 */
        mx = MAX(0, MIN(sw - 1, (int)((int64_t)ev->dx * sw / 32768)));
        my = MAX(0, MIN(sh - 1, (int)((int64_t)ev->dy * sh / 32768)));
    } else {
        mx = MAX(0, MIN(sw - 1, mx + ax * mult[sp] / 2));
        my = MAX(0, MIN(sh - 1, my + ay * mult[sp] / 2));
    }
    bool left_now = ev->buttons & MOUSE_LEFT, left_before = mbuttons & MOUSE_LEFT;

    if (left_now && !left_before)
        mouse_click(mx, my);
    if (!left_now)
        drag_idx = -1;
    if (left_now && drag_idx >= 0 && wins[drag_idx].used) {
        wins[drag_idx].x = MAX(-wins[drag_idx].cw + 60, MIN(sw - 60, mx - drag_dx));
        wins[drag_idx].y = MAX(MENUBAR_H, MIN(sh - DOCK_RESERVE, my - drag_dy));
    }
    mbuttons = ev->buttons;
}

static void cycle_windows(void)
{
    if (nz > 1)
        bring_to_front(zorder[0]);
}

static void handle_key(const struct input_event *ev)
{
    int k = ev->key;

    if (ev->mods & MOD_ALT) {
        if (k == ' ') { spot_toggle(); return; }
        if (k == '\t') { cycle_windows(); return; }
        if (k == 'q' || k == 'Q') { gui_quit(); return; }
        if (k == 'm' || k == 'M') { menu_open = menu_open ? M_NONE : M_APPLE; return; }
        if (k == 'w' || k == 'W' || k == KEY_F(4)) {
            if (focused())
                gui_close(focused());
            return;
        }
        int step = (ev->mods & MOD_SHIFT) ? 64 : 16;
        switch (k) {
        case KEY_LEFT:  mx = MAX(0, mx - step); return;
        case KEY_RIGHT: mx = MIN(sw - 1, mx + step); return;
        case KEY_UP:    my = MAX(0, my - step); return;
        case KEY_DOWN:  my = MIN(sh - 1, my + step); return;
        case '\n':      mouse_click(mx, my); return;
        }
        if (k >= '1' && k <= '9' && k - '1' < ndock) {      /* Alt+1.. = dock position */
            launch(apps[dock_idx[k - '1']]);
            return;
        }
        for (int i = 0; i < ndock; i++) {                   /* Alt+letter = first dock app with it */
            char c = apps[dock_idx[i]]->name[0];
            if (tolower(c) == tolower(k)) {
                launch(apps[dock_idx[i]]);
                return;
            }
        }
    }
    if (k == KEY_F(2)) { ptr_mode = !ptr_mode; return; }
    if (spot_open) {
        spot_key(k);
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
            menu_open = M_NONE;
        return;
    }
    struct gui_window *w = focused();
    if (w && w->app->key)
        w->app->key(w, k, ev->mods);
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

bool gui_supported(void) { return gfx_available(); }

static void guitest_task(void *arg)
{
    (void)arg;
    const char *cmd = "echo hi\n";
    struct input_event ev;

    timer_sleep_ms(2500);
    for (const char *c = cmd; *c; c++) {
        ev = (struct input_event){ .type = INPUT_KEY, .key = *c };
        input_push(&ev);
        timer_sleep_ms(100);
    }
    timer_sleep_ms(800);
    ev = (struct input_event){ .type = INPUT_KEY, .key = 'f', .mods = MOD_ALT };
    input_push(&ev);
    timer_sleep_ms(500);
    ev = (struct input_event){ .type = INPUT_KEY, .key = 's', .mods = MOD_ALT };
    input_push(&ev);
    timer_sleep_ms(1500);
    if (boot_has_arg("fbdump"))
        gfx_dump_serial();
    ev = (struct input_event){ .type = INPUT_KEY, .key = 'q', .mods = MOD_ALT };
    input_push(&ev);
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
    menu_open = M_NONE;
    spot_open = false;
    ptr_mode = false;
    running = true;
    mx = sw / 2;
    my = sh / 2;
    mbuttons = 0;
    drag_idx = -1;
    gui_th = gui_prefs.dark ? &gui_theme_dark : &gui_theme_light;
    tg_light_mode = !gui_prefs.dark;
    collect_apps();
    build_dock();
    build_menu();

    gui_splash();
    build_wallpaper();
    input_flush();

    gui_open_named("Terminal");
    struct gui_window *term = nz ? &wins[zorder[nz - 1]] : NULL;
    if (term) {
        term->x = 40;
        term->y = MENUBAR_H + 16;
    }
    struct gui_window *info = gui_open_named("Activity");
    if (info) {
        info->x = MAX(20, sw - outer_w(info) - 30);
        info->y = MENUBAR_H + 40;
        if (term)
            bring_to_front(win_index(term));
    }
    if (boot_has_arg("guitest"))
        task_create("guitest", guitest_task, NULL);

    uint32_t last_sec = ~0u;
    while (running) {
        struct input_event ev;
        bool redraw = need_redraw;
        int r = input_wait(&ev, animating ? 30 : 100);

        need_redraw = false;
        while (r == 0) {
            if (ev.type == INPUT_MOUSE)
                handle_mouse(&ev);
            else if (ev.type == INPUT_KEY)
                handle_key(&ev);
            redraw = true;
            r = input_poll(&ev) ? 0 : -1;
        }
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (wins[i].used && wins[i].app->tick)
                wins[i].app->tick(&wins[i]);
        reap_windows();
        uint32_t sec = timer_uptime_sec();
        if (sec != last_sec) {
            last_sec = sec;
            redraw = true;
        }
        if (animating || need_redraw)
            redraw = true;
        if (redraw) {
            need_redraw = false;
            render();
        }
    }

    if (boot_has_arg("guitest"))
        serial_write("GUITEST done\n");
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used)
            destroy_window(&wins[i]);
    nz = 0;
    console_set_key_source(NULL);
    console_set_backend(&fbcon_console);
    gfx_surface_free(wallpaper);
    wallpaper = NULL;
    input_flush();
}
