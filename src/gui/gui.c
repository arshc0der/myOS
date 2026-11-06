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
#include "display.h"
#include "pmm.h"
#include "sys.h"
#include "image.h"
#include "net.h"
#include "panels.h"
#include "notify.h"
#include "camera.h"
#include "vfs.h"
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
static void win_resize(struct gui_window *w, int cw, int ch);
static int mx, my;
static uint8_t mbuttons;
static bool running, animating, need_redraw;
/* animation clock: milliseconds from the CPU time-stamp counter (the 100 Hz tick would make motion step in 10 ms jumps) */
static uint32_t anim_now(void) { return timer_us() / 1000; }
static int gdt_ms = 16;                         /* time since the previous frame, for delta-time animation */
static bool ptr_mode;

/* menus */
enum { M_NONE, M_APPLE, M_APP, M_POPUP, M_NAME, M_WINDOW, M_STATUS };
static int menu_open;
struct mitem { char label[48]; int code; bool dis; };   /* code: >=0 open app, <0 special, -9 separator */
static struct mitem mitems[MAX_APPS + 8];               /* the MyOS (Apple) menu */
static int nmitems;
/* the dropdown that is open right now: MyOS menu, an app menu from the menu bar, or a popup */
static struct {
    struct mitem it[MAX_APPS + 8];
    int n, x, y, w;
    int owner;                                           /* window index, -1 = the system menu */
    int which;                                           /* which app menu (M_APP) */
} dd;
static int appmenu_x[8], appmenu_w[8];                  /* title rectangles in the menu bar */

/* modal dialog (prompt / confirm / alert) */
static struct {
    bool open;
    int kind;                                            /* 0 prompt, 1 confirm, 2 alert */
    int owner, id;
    char title[40], msg[96], text[64];
    bool fresh;                     /* prompt text untouched: the first key replaces it */
} dlg;

/* clipboard */
static char clip[256];
static int clip_kind;
#define MI_QUIT -1
#define MI_RESTART -2
#define MI_SHUTDOWN -3
#define MI_SEP -9
#define MI_APP_ABOUT -10
#define MI_APP_HIDE -11
#define MI_APP_QUIT -12
#define MI_WIN_MIN -20
#define MI_WIN_ZOOM -21
#define MI_WIN_FS -22
#define MI_WIN_CLOSE -23
#define MI_NET_ON -30
#define MI_NET_SETTINGS -31
#define MI_BT_SETTINGS -32
#define MI_D_TERM -40
#define MI_D_FILES -41
#define MI_D_WALL -42
#define MI_D_DISPLAY -43
#define MI_D_CC -44
#define MI_D_NC -45
#define MI_D_LOCK -46
#define MI_D_SLEEP -47
#define MI_WIN_FOCUS -200               /* MI_WIN_FOCUS - window index */
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

static struct gui_window *focused(void)
{
    for (int i = nz - 1; i >= 0; i--)
        if (!wins[zorder[i]].minimized)
            return &wins[zorder[i]];
    return NULL;
}

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
    w->born = anim_now();
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

static char wall_path[96];
static bool wallpaper_ok;
static int wall_style;                                  /* colour preset when no picture is chosen */
static int wall_mode;                                   /* 0 fill (crop), 1 fit, 2 stretch, 3 centre */

#define RGB(r, g, b) GFX_RGB(r, g, b)
static const struct { const char *name; uint32_t top, bot; } wp_presets[GUI_WALL_PRESETS] = {
    { "Aurora", RGB(32, 201, 151), RGB(95, 61, 196) },    { "Sunset", RGB(255, 153, 102), RGB(214, 51, 132) },
    { "Ocean", RGB(77, 171, 247), RGB(11, 60, 130) },     { "Forest", RGB(81, 207, 102), RGB(18, 64, 43) },
    { "Midnight", RGB(41, 50, 90), RGB(8, 10, 24) },      { "Graphite", RGB(134, 142, 150), RGB(33, 37, 41) },
    { "Rose", RGB(250, 162, 193), RGB(166, 30, 77) },     { "Lavender", RGB(208, 191, 255), RGB(103, 65, 217) },
    { "Sand", RGB(255, 236, 179), RGB(201, 150, 70) },    { "Mint", RGB(195, 250, 232), RGB(18, 184, 134) },
    { "Fire", RGB(255, 212, 59), RGB(224, 49, 49) },      { "Sky", RGB(165, 216, 255), RGB(34, 139, 230) },
    { "Blue", RGB(28, 100, 242), RGB(28, 100, 242) },     { "Green", RGB(47, 158, 68), RGB(47, 158, 68) },
    { "Purple", RGB(112, 72, 232), RGB(112, 72, 232) },   { "Orange", RGB(240, 140, 0), RGB(240, 140, 0) },
    { "Black", RGB(8, 8, 10), RGB(8, 8, 10) },            { "White", RGB(240, 242, 246), RGB(240, 242, 246) },
};
#undef RGB

int gui_wallpaper_preset_count(void) { return GUI_WALL_PRESETS; }
const char *gui_wallpaper_preset_name(int i) { return i >= 0 && i < GUI_WALL_PRESETS ? wp_presets[i].name : ""; }
uint32_t gui_wallpaper_preset_color(int i, bool bottom)
{
    if (i < 0 || i >= GUI_WALL_PRESETS)
        return 0;
    return bottom ? wp_presets[i].bot : wp_presets[i].top;
}

/* Place a picture: fill (centre crop), fit (whole picture, bars), stretch or centre (actual size). */
static bool wallpaper_from_picture(void)
{
    const char *err;
    struct gfx_surface *raw = image_load(wall_path, gui_th->bg_top, &err);
    if (!raw)
        return false;
    uint32_t bar = gfx_blend(gui_th->bg_top, GFX_BLACK, 200);
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            int sx, sy;
            bool inside = true;
            if (wall_mode == 2) {                                    /* stretch */
                sx = x * raw->w / sw;
                sy = y * raw->h / sh;
            } else if (wall_mode == 3) {                             /* centre, no scaling */
                sx = x - (sw - raw->w) / 2;
                sy = y - (sh - raw->h) / 2;
                inside = sx >= 0 && sy >= 0 && sx < raw->w && sy < raw->h;
            } else {
                bool wider = (int64_t)raw->w * sh >= (int64_t)raw->h * sw;     /* picture wider than the screen? */
                bool by_h = wall_mode == 1 ? !wider : wider;                  /* which side decides the scale */
                if (by_h) {                                          /* scale so the heights match */
                    sx = (int)((int64_t)(x - (sw - (int64_t)raw->w * sh / raw->h) / 2) * raw->h / sh);
                    sy = y * raw->h / sh;
                } else {                                             /* scale so the widths match */
                    sx = x * raw->w / sw;
                    sy = (int)((int64_t)(y - (sh - (int64_t)raw->h * sw / raw->w) / 2) * raw->w / sw);
                }
                inside = sx >= 0 && sy >= 0 && sx < raw->w && sy < raw->h;
            }
            wallpaper->px[y * wallpaper->stride + x] = inside ? raw->px[sy * raw->stride + sx] : bar;
        }
    }
    gfx_surface_free(raw);
    return true;
}

static void build_wallpaper(void)
{
    gfx_surface_free(wallpaper);
    wallpaper = gfx_surface_create(sw, sh);
    if (!wallpaper)
        return;
    wallpaper_ok = false;
    if (wall_path[0] && wallpaper_from_picture()) {
        wallpaper_ok = true;
        return;
    }
    if (wall_style > 0 && wall_style <= GUI_WALL_PRESETS) {
        uint32_t t = wp_presets[wall_style - 1].top, b = wp_presets[wall_style - 1].bot;
        gfx_gradient_v(wallpaper, 0, 0, sw, sh, t, b);
        if (t != b) {
            gfx_fill_circle(wallpaper, sw * 78 / 100, sh * 30 / 100, sh / 3, gfx_blend(t, b, 90));
            gfx_fill_circle(wallpaper, sw * 22 / 100, sh * 80 / 100, sh / 4, gfx_blend(t, b, 170));
        }
        return;
    }
    gfx_gradient_v(wallpaper, 0, 0, sw, sh, gui_th->bg_top, gui_th->bg_bot);
    uint32_t c1 = gfx_blend(gui_th->bg_top, gui_th->bg_bot, 80), c2 = gfx_blend(gui_th->bg_top, gui_th->bg_bot, 150);
    gfx_fill_circle(wallpaper, sw * 78 / 100, sh * 30 / 100, sh / 3, c1);
    gfx_fill_circle(wallpaper, sw * 22 / 100, sh * 80 / 100, sh / 4, c2);
}

const struct gfx_surface *gui_wallpaper_surface(void) { return wallpaper; }
int gui_wallpaper_style(void) { return wall_style; }
int gui_wallpaper_mode(void) { return wall_mode; }
const char *gui_wallpaper_path(void) { return wall_path; }

static void wall_save(void)
{
    sys_prefs.wall_style = wall_style;
    sys_prefs.wall_mode = wall_mode;
    strlcpy(sys_prefs.wall, wall_path, sizeof sys_prefs.wall);
    sys_prefs_save();
}

/* Cross-fade from the old picture to the new one */
static struct gfx_surface *wall_old;
static uint32_t wall_old_t0;

static void wall_changed(void)
{
    struct gfx_surface *old = wallpaper;

    wallpaper = NULL;
    build_wallpaper();
    gfx_surface_free(wall_old);
    wall_old = NULL;
    if (gui_prefs.anim && old && wallpaper && old->w == wallpaper->w && old->h == wallpaper->h) {
        wall_old = old;
        wall_old_t0 = anim_now();
        animating = true;
    } else
        gfx_surface_free(old);
    wall_save();
    gui_redraw();
}

bool gui_set_wallpaper(const char *path)
{
    char old[sizeof wall_path];
    int os = wall_style;

    strlcpy(old, wall_path, sizeof old);
    strlcpy(wall_path, path, sizeof wall_path);
    wall_style = 0;
    struct gfx_surface *keep = wallpaper;
    wallpaper = NULL;
    build_wallpaper();
    if (path[0] && !wallpaper_ok) {
        strlcpy(wall_path, old, sizeof wall_path);
        wall_style = os;
        gfx_surface_free(wallpaper);
        wallpaper = keep;
        return false;
    }
    wallpaper = keep;
    wall_changed();
    return true;
}

void gui_set_wallpaper_style(int style)
{
    wall_path[0] = 0;
    wall_style = style < 0 || style > GUI_WALL_PRESETS ? 0 : style;
    wall_changed();
}

void gui_set_wallpaper_mode(int mode)
{
    wall_mode = mode < 0 || mode > 3 ? 0 : mode;
    if (wall_path[0])
        wall_changed();
    else
        wall_save();
}

/* After the saved settings are known (login): take them over. */
static void wall_apply_saved(void)
{
    if (!strcmp(wall_path, sys_prefs.wall) && wall_style == sys_prefs.wall_style && wall_mode == sys_prefs.wall_mode)
        return;
    strlcpy(wall_path, sys_prefs.wall, sizeof wall_path);
    wall_style = sys_prefs.wall_style;
    wall_mode = sys_prefs.wall_mode;
    build_wallpaper();
    if (wall_path[0] && !wallpaper_ok) {                   /* the picture is gone: go back to the default */
        wall_path[0] = 0;
        build_wallpaper();
    }
    gui_redraw();
}

void gui_refresh_theme(void)
{
    gui_theme_apply();
    tg_light_mode = !gui_prefs.dark;
    build_wallpaper();
    gui_redraw();
}

static struct gfx_surface *snap;                        /* old look, fading out after a theme change */
static uint32_t snap_t0;

void gui_request_frames(void)
{
    animating = true;
    need_redraw = true;
}

void gui_set_dark(bool dark)
{
    if (gui_prefs.anim && gfx_available()) {
        struct gfx_surface *sc = gfx_screen();
        if (!snap || snap->w != sc->w || snap->h != sc->h) {
            if (snap)
                gfx_surface_free(snap);
            snap = gfx_surface_create(sc->w, sc->h);
        }
        if (snap) {
            for (int y = 0; y < sc->h; y++)
                memcpy(snap->px + y * snap->stride, sc->px + y * sc->stride, (size_t)sc->w * 4);
            snap_t0 = timer_us() / 1000;
            gui_request_frames();
        }
    }
    gui_prefs.dark = dark;
    sys_prefs.dark = dark;
    sys_prefs_save();
    gui_refresh_theme();
}

/* ------------------------------------------------------------------ */
/* Icons and the dock                                                  */
/* ------------------------------------------------------------------ */

#define DOCK_ICON (gui_prefs.dock_size)
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

static int dock_off;                                    /* pixels the dock is pushed below the screen edge (auto-hide) */

static void dock_rect(int *x, int *y, int *w, int *h)
{
    *w = ndock * DOCK_ICON + (ndock - 1) * DOCK_GAP + 32;
    *h = DOCK_ICON + 14;
    *x = (sw - *w) / 2;
    *y = sh - *h - 8 + dock_off;
}

/* Where the dock wants to be: 0 = shown, otherwise how far below the screen. */
static int dock_target(void)
{
    int mode = sys_prefs.dock_hide, hh = DOCK_ICON + 14 + 14;

    if (mode == 0 || !ndock)
        return 0;
    bool hide = mode == 2;
    for (int i = 0; i < nz && !hide; i++) {
        const struct gui_window *w = &wins[zorder[i]];
        if (w->used && !w->minimized && (w->maximized || w->fullscreen))
            hide = true;
    }
    if (!hide)
        return 0;
    int dw = ndock * DOCK_ICON + (ndock - 1) * DOCK_GAP + 32, dx = (sw - dw) / 2;
    if (my >= sh - 3)                                   /* pointer pushed against the bottom edge */
        return 0;
    if (dock_off < hh / 2 && my >= sh - DOCK_ICON - 36 && mx >= dx - 30 && mx < dx + dw + 30)
        return 0;                                       /* already showing and the pointer is still on it */
    return hh;
}

static void dock_slide(void)
{
    int target = dock_target();

    if (dock_off == target)
        return;
    if (!gui_prefs.anim) {
        dock_off = target;
        return;
    }
    int diff = target - dock_off, step = diff * MIN(gdt_ms, 50) / 90;       /* ease-out, delta-time based */
    if (!step)
        step = diff > 0 ? 1 : -1;
    dock_off += step;
    if ((diff > 0 && dock_off > target) || (diff < 0 && dock_off < target))
        dock_off = target;
    animating = true;
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
                dock_bounce[i] = anim_now() | 1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used && wins[i].app == a && wins[i].minimized && !wins[i].want_close) {
            wins[i].minimized = false;                 /* bring a hidden window back */
            bring_to_front(i);
            wins[i].born = anim_now();
            gui_redraw();
            return;
        }
    gui_open(a);
}

static struct gfx_surface *icon_img[MAX_APPS];          /* custom pictures, indexed like apps[] */

static const uint32_t tile_colors[8][2] = {             /* top, bottom of each coloured tile */
    { 0x5AC8FA, 0x0A84FF }, { 0x7BE495, 0x28A745 }, { 0xFFB547, 0xFF7A00 }, { 0xC084FC, 0x7C3AED },
    { 0xFF8FB1, 0xE5266B }, { 0xFF7B72, 0xD92D20 }, { 0x5EEAD4, 0x0D9488 }, { 0x8E9BFF, 0x4F46E5 },
};

void gui_reload_icons(void)
{
    static const char *const ext[] = { ".png", ".bmp", ".jpg", ".jpeg" };
    char p[VFS_PATH_MAX];
    const char *err;

    for (int i = 0; i < MAX_APPS; i++) {
        gfx_surface_free(icon_img[i]);
        icon_img[i] = NULL;
    }
    for (int i = 0; i < napps; i++)
        for (int d = 0; d < 2 && !icon_img[i]; d++)
            for (unsigned e = 0; e < 4 && !icon_img[i]; e++) {
                struct vfs_stat st;

                if (d == 0 && !users_current())
                    continue;
                if (d == 0)
                    ksnprintf(p, sizeof p, "%s/Library/Icons/%s%s", users_home(), apps[i]->name, ext[e]);
                else
                    ksnprintf(p, sizeof p, "/System/Icons/%s%s", apps[i]->name, ext[e]);
                if (vfs_stat(p, &st) < 0)
                    continue;
                struct gfx_surface *raw = image_load(p, gui_th->tile, &err);
                if (raw) {
                    icon_img[i] = image_fit(raw, 96, 96);
                    gfx_surface_free(raw);
                }
            }
    gui_redraw();
}

static void draw_app_icon(struct gfx_surface *s, const struct gui_app *a, int x, int y, int sz)
{
    uint32_t tile = gui_th->tile, fg = gui_th->glyph, edge = gui_th->tile_edge;
    int idx = 0;

    for (int i = 0; i < napps; i++)
        if (apps[i] == a)
            idx = i;
    if (a->dark_tile) {
        tile = GFX_RGB(16, 16, 18);
        fg = GFX_RGB(248, 248, 250);
        edge = gui_prefs.dark ? GFX_RGB(110, 110, 118) : tile;
    } else if (gui_prefs.dock_colorful && gui_prefs.accent != GUI_ACCENTS - 1 && !icon_img[idx]) {
        unsigned h = 0;
        for (const char *c = a->name; *c; c++)
            h = h * 31 + (uint8_t)*c;
        tile = tile_colors[h % 8][1];
        edge = tile;
        fg = GFX_WHITE;
    }
    gfx_fill_round_rect(s, x - 1, y - 1, sz + 2, sz + 2, sz * 24 / 100, edge);
    gfx_fill_round_rect(s, x, y, sz, sz, sz * 23 / 100, tile);
    if (icon_img[idx]) {                                    /* the user's own picture */
        const struct gfx_surface *im = icon_img[idx];
        int box = sz * 90 / 100, ox = x + (sz - box) / 2, oy = y + (sz - box) / 2;
        for (int j = 0; j < box; j++)
            for (int i = 0; i < box; i++) {
                int sx = i * im->w / box, sy = j * im->h / box;
                gfx_pixel(s, ox + i, oy + j, im->px[sy * im->stride + sx]);
            }
        return;
    }
    if (a->icon)
        a->icon(s, x, y, sz, fg, tile);
    else
        gfx_char(s, x + sz / 2 - 8, y + sz / 2 - 8, a->name[0], fg, GFX_TRANSPARENT, 2);
}

static void draw_dock(struct gfx_surface *s)
{
    int x, y, w, h;
    uint32_t now = anim_now();

    if (!ndock)
        return;
    dock_slide();
    dock_rect(&x, &y, &w, &h);
    if (dock_off >= h + 10)
        return;                                         /* fully hidden */
    gfx_fill_round_rect(s, x - 1, y - 1, w + 2, h + 2, 19, gui_th->line);
    gfx_fill_round_rect(s, x, y, w, h, 18, gfx_blend(gfx_get_pixel(s, x + w / 2, y + 2), gui_th->menu, 215));
    for (int i = 0; i < ndock; i++) {
        const struct gui_app *a = apps[dock_idx[i]];
        int ix = dock_icon_x(i);
        bool hot = mx >= ix - 4 && mx < ix + DOCK_ICON + 4 && my >= y - 6 && my < y + h;
        bool near = mx >= x && mx < x + w && my >= y - 12;
        int dist = mx - (ix + DOCK_ICON / 2);
        if (dist < 0)
            dist = -dist;
        int target = near && dist < 96 ? 16 * (96 - dist) / 96 : 0;    /* neighbours grow too, like macOS */
        if (!gui_prefs.anim)
            dock_sc[i] = target;
        else if (dock_sc[i] != target) {
            int diff = target - dock_sc[i], step = diff * MIN(gdt_ms, 50) / 70;     /* delta-time ease-out */
            dock_sc[i] += step ? step : (diff > 0 ? 1 : -1);
            animating = true;
        }
        int sz = DOCK_ICON + dock_sc[i];
        int dx = ix - (sz - DOCK_ICON) / 2, dy = y + 7 - dock_sc[i];
        if (dock_bounce[i]) {
            int dt = (int)(now - dock_bounce[i]);                           /* milliseconds */
            if (dt < 520) {
                dy -= 4 * 18 * dt * (520 - dt) / (520 * 520);               /* one smooth hop, 18 px high */
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

    if (w->fullscreen) {                                /* no chrome: only the client area */
        if (w->app->draw)
            w->app->draw(w);
        gfx_blit(s, 0, 0, w->surf, 0, 0, w->cw, w->ch);
        return;
    }
    if (gui_prefs.anim && !w->maximized) {                               /* open animation: slide up, 200 ms */
        int t = (int)(anim_now() - w->born);                         /* milliseconds */
        if (t < 240) {
            int rem = 240 - t;                                      /* ease-out cubic: fast start, soft landing */
            w->y += 36 * rem * rem * rem / (240 * 240 * 240 / 1);
            animating = true;
        }
    }
    for (int k = active ? 5 : 2; k >= 0; k--)               /* big soft shadow only for the front window */
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
    if (mx >= w->x + 8 && mx < w->x + 72 && my >= w->y + 3 && my < w->y + 27) {     /* hovering the buttons: show symbols */
        uint32_t g = GFX_RGB(70, 20, 16);
        int cy = w->y + 15, c0 = w->x + 18, c1 = w->x + 38, c2 = w->x + 58;
        gfx_line(s, c0 - 2, cy - 2, c0 + 2, cy + 2, g);                 /* close: x */
        gfx_line(s, c0 - 2, cy + 2, c0 + 2, cy - 2, g);
        gfx_hline(s, c1 - 3, cy, 7, g);                                 /* minimize: - */
        if (w->maximized) {                                             /* restore: two squares */
            gfx_rect(s, c2 - 3, cy - 1, 5, 5, g);
            gfx_rect(s, c2 - 1, cy - 3, 5, 5, g);
        } else
            gfx_rect(s, c2 - 3, cy - 3, 7, 7, g);                       /* zoom: square */
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

static void dd_close(void) { menu_open = M_NONE; dd.n = 0; }

static void dd_add(const char *label, int code)
{
    if (dd.n >= (int)ARRAY_SIZE(dd.it))
        return;
    struct mitem *m = &dd.it[dd.n++];
    m->dis = false;
    if (!label || !strcmp(label, "-")) {
        m->label[0] = '\0';
        m->code = MI_SEP;
        return;
    }
    if (label[0] == '~') {
        m->dis = true;
        label++;
    }
    strlcpy(m->label, label, sizeof m->label);
    m->code = code;
}

static void dd_size(int minw)
{
    int w = minw;

    for (int i = 0; i < dd.n; i++)
        w = MAX(w, gfx_text_width(dd.it[i].label, 1) + 44);
    dd.w = w;
    if (dd.x + dd.w > sw - 4)
        dd.x = MAX(4, sw - 4 - dd.w);
    int hh = 10;
    for (int i = 0; i < dd.n; i++)
        hh += dd.it[i].code == MI_SEP ? 9 : MENU_ITEM_H;
    if (dd.y + hh > sh - 4)
        dd.y = MAX(MENUBAR_H, sh - 4 - hh);
}

static void open_apple_menu(void)
{
    dd.n = 0;
    dd.owner = -1;
    dd.x = 8;
    dd.y = MENUBAR_H + 2;
    for (int i = 0; i < nmitems; i++) {
        dd_add(mitems[i].code == MI_SEP ? "-" : mitems[i].label, mitems[i].code);
    }
    dd_size(MENU_W);
    menu_open = M_APPLE;
}

static void open_app_menu(struct gui_window *w, int which)
{
    const struct gui_menu *m = &w->app->menus[which];

    dd.n = 0;
    dd.owner = win_index(w);
    dd.which = which;
    dd.x = appmenu_x[which];
    dd.y = MENUBAR_H + 2;
    for (int i = 0; i < m->n; i++)
        dd_add(m->items[i].label, m->items[i].id);
    dd_size(170);
    menu_open = M_APP;
}

static int name_x, name_w, win_x, win_w, net_x = 9999, bt_x = 9999, srch_x = 9999, cc_x = 9999, clock_x = 9999;

static void open_name_menu(struct gui_window *f)
{
    char l[48];

    dd.n = 0;
    dd.owner = -1;
    dd.x = name_x;
    dd.y = MENUBAR_H + 2;
    ksnprintf(l, sizeof l, "About %s", f->app->name);
    dd_add(l, MI_APP_ABOUT);
    dd_add("-", 0);
    ksnprintf(l, sizeof l, "Hide %s", f->app->name);
    dd_add(l, MI_APP_HIDE);
    ksnprintf(l, sizeof l, "Quit %s", f->app->name);
    dd_add(l, MI_APP_QUIT);
    dd_size(180);
    menu_open = M_NAME;
}

static void open_window_menu(struct gui_window *f)
{
    dd.n = 0;
    dd.owner = -1;
    dd.x = win_x;
    dd.y = MENUBAR_H + 2;
    dd_add("Minimize", MI_WIN_MIN);
    dd_add(f->maximized ? "Restore" : "Zoom", MI_WIN_ZOOM);
    dd_add(f->fullscreen ? "Exit Full Screen  (F11)" : "Enter Full Screen  (F11)", MI_WIN_FS);
    dd_add("Close Window", MI_WIN_CLOSE);
    dd_add("-", 0);
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used && !wins[i].want_close) {
            char l[48];
            ksnprintf(l, sizeof l, "%s%s", &wins[i] == f ? "> " : "  ", wins[i].title);
            dd_add(l, MI_WIN_FOCUS - i);
        }
    dd_size(200);
    menu_open = M_WINDOW;
}

static void open_status_menu(int which)
{
    char l[48];

    dd.n = 0;
    dd.owner = -1;
    dd.which = which;
    dd.x = (which == 0 ? net_x : bt_x) - 6;
    dd.y = MENUBAR_H + 2;
    if (which == 0) {
        ksnprintf(l, sizeof l, "~%s", net_status_text());
        dd_add(l, 0);
        dd_add("-", 0);
        if (!net_is_up() && !net_busy())
            dd_add("Turn Network On", MI_NET_ON);
        dd_add("Network Settings...", MI_NET_SETTINGS);
    } else {
        ksnprintf(l, sizeof l, "~Bluetooth: %d adapter(s)", bt_count());
        dd_add(l, 0);
        dd_add("-", 0);
        dd_add("Bluetooth Settings...", MI_BT_SETTINGS);
    }
    dd_size(220);
    menu_open = M_STATUS;
}

void gui_popup(struct gui_window *w, int x, int y, const struct gui_menu_item *items, int n)
{
    dd.n = 0;
    dd.owner = win_index(w);
    dd.x = w->x + 1 + x;
    dd.y = w->y + TITLE_H + y;
    for (int i = 0; i < n; i++)
        dd_add(items[i].label, items[i].id);
    dd_size(170);
    menu_open = M_POPUP;
    gui_redraw();
}

static void menu_rect(int *x, int *y, int *w, int *h)
{
    int hh = 10;

    for (int i = 0; i < dd.n; i++)
        hh += dd.it[i].code == MI_SEP ? 9 : MENU_ITEM_H;
    *x = dd.x; *y = dd.y; *w = dd.w; *h = hh;
}

static void draw_menu(struct gfx_surface *s)
{
    int x, y, w, h;

    menu_rect(&x, &y, &w, &h);
    gfx_fill_round_rect(s, x + 2, y + 4, w, h, 10, gfx_blend(gfx_get_pixel(s, x + 4, MIN(sh - 1, y + h + 6)), GFX_BLACK, 40));
    gfx_fill_round_rect(s, x - 1, y - 1, w + 2, h + 2, 11, gui_th->line);
    gfx_fill_round_rect(s, x, y, w, h, 10, gui_th->menu);
    int iy = y + 5;
    for (int i = 0; i < dd.n; i++) {
        if (dd.it[i].code == MI_SEP) {
            gfx_hline(s, x + 12, iy + 4, w - 24, gui_th->line);
            iy += 9;
            continue;
        }
        bool hot = !dd.it[i].dis && mx >= x && mx < x + w && my >= iy && my < iy + MENU_ITEM_H;
        if (hot)
            gfx_fill_round_rect(s, x + 5, iy, w - 10, MENU_ITEM_H - 2, 6, gui_th->accent);
        gfx_text(s, x + 16, iy + 9, dd.it[i].label,
                 dd.it[i].dis ? gui_th->dim : (hot ? gui_th->accent_fg : gui_th->text), GFX_TRANSPARENT, 1);
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
    for (int i = 0; i < dd.n; i++) {
        int ih = dd.it[i].code == MI_SEP ? 9 : MENU_ITEM_H;
        if (py >= iy && py < iy + ih)
            return (dd.it[i].code == MI_SEP || dd.it[i].dis) ? -3 : i;
        iy += ih;
    }
    return -2;
}

static void lock_now(void)
{
    if (users_count() == 0 || !sys_prefs.setup_done || boot_has_arg("guitest") || boot_has_arg("nosetup"))
        return;
    gfx_set_brightness(MAX(10, MIN(100, sys_prefs.brightness)));
    gui_lock_screen();
    input_flush();
    mbuttons = 0;
    need_redraw = true;
}

/* Sleep: the picture fades to black; any key or pointer movement wakes it, then the lock screen asks for the password. */
static void gui_sleep(void)
{
    struct gfx_surface *sc = gfx_screen();
    int b = MAX(10, MIN(100, sys_prefs.brightness));
    struct input_event ev;

    for (int i = 1; i <= 8; i++) {
        gfx_set_brightness(b - (b - 10) * i / 8);
        gfx_flush_all();
        timer_sleep_ms(30);
    }
    gfx_fill_rect(sc, 0, 0, sc->w, sc->h, GFX_BLACK);
    gfx_flush_all();
    input_flush();
    for (;;) {
        if (input_wait(&ev, 500) != 0)
            continue;
        if (ev.type == INPUT_KEY || (ev.type == INPUT_MOUSE && (ev.dx || ev.dy || ev.buttons)))
            break;
    }
    input_flush();
    gfx_set_brightness(b);
    mbuttons = 0;
    need_redraw = true;
    lock_now();
}

/* Right click on the desktop */
static void open_desktop_menu(int x, int y)
{
    dd.n = 0;
    dd.owner = -1;
    dd_add("Open Terminal", MI_D_TERM);
    dd_add("Open Files", MI_D_FILES);
    dd_add("-", 0);
    dd_add("Change Wallpaper...", MI_D_WALL);
    dd_add("Display Settings...", MI_D_DISPLAY);
    dd_add("Control Center", MI_D_CC);
    dd_add("Notifications and Calendar", MI_D_NC);
    dd_add("-", 0);
    if (users_count() > 0 && sys_prefs.setup_done)
        dd_add("Lock Screen", MI_D_LOCK);
    dd_add("Sleep", MI_D_SLEEP);
    dd_add("Restart...", MI_RESTART);
    dd_add("Shut Down...", MI_SHUTDOWN);
    dd_size(230);
    int mw, mh, hh = 10;
    (void)mw;
    for (int i = 0; i < dd.n; i++)
        hh += dd.it[i].code == MI_SEP ? 9 : MENU_ITEM_H;
    mh = hh;
    dd.x = MAX(4, MIN(x, sw - dd.w - 4));
    dd.y = MAX(MENUBAR_H + 2, MIN(y, sh - mh - 4));
    menu_open = M_POPUP;
}

static void menu_action(int i)
{
    int code = dd.it[i].code, owner = dd.owner;

    dd_close();
    if (owner >= 0) {                                   /* an app menu or a popup */
        struct gui_window *w = &wins[owner];
        if (w->used && w->app->command)
            w->app->command(w, code);
        gui_redraw();
        return;
    }
    struct gui_window *fw = focused();
    if (code >= 0)
        launch(apps[code]);
    else if (code == MI_APP_ABOUT)
        gui_open_named("About");
    else if (code == MI_APP_HIDE && fw) {
        for (int k = 0; k < MAX_WINDOWS; k++)
            if (wins[k].used && wins[k].app == fw->app)
                wins[k].minimized = true;
    } else if (code == MI_APP_QUIT && fw) {
        for (int k = 0; k < MAX_WINDOWS; k++)
            if (wins[k].used && wins[k].app == fw->app)
                gui_close(&wins[k]);
    } else if (code == MI_WIN_MIN && fw)
        gui_minimize(fw);
    else if (code == MI_WIN_ZOOM && fw)
        gui_maximize(fw);
    else if (code == MI_WIN_FS && fw)
        gui_fullscreen(fw);
    else if (code == MI_WIN_CLOSE && fw)
        gui_close(fw);
    else if (code <= MI_WIN_FOCUS && MI_WIN_FOCUS - code < MAX_WINDOWS) {
        int k = MI_WIN_FOCUS - code;
        if (wins[k].used) {
            wins[k].minimized = false;
            bring_to_front(k);
        }
    } else if (code == MI_NET_ON)
        net_start_async();
    else if (code == MI_NET_SETTINGS)
        settings_show("Network");
    else if (code == MI_BT_SETTINGS)
        settings_show("Bluetooth");
    else if (code == MI_D_TERM)
        gui_open_named("Terminal");
    else if (code == MI_D_FILES)
        gui_open_named("Files");
    else if (code == MI_D_WALL)
        settings_show("Appearance");
    else if (code == MI_D_DISPLAY)
        settings_show("Display");
    else if (code == MI_D_CC)
        panels_toggle(PANEL_CONTROL);
    else if (code == MI_D_NC)
        panels_toggle(PANEL_NOTIFY);
    else if (code == MI_D_LOCK)
        lock_now();
    else if (code == MI_D_SLEEP)
        gui_sleep();
    else if (code == MI_QUIT)
        gui_quit();
    else if (code == MI_SHUTDOWN)
        system_poweroff();
    else if (code == MI_RESTART)
        system_reboot();
}

static void draw_bt_icon(struct gfx_surface *s, int x, int y, uint32_t c)
{
    gfx_line(s, x + 4, y, x + 4, y + 12, c);
    gfx_line(s, x + 4, y, x + 8, y + 3, c);
    gfx_line(s, x + 8, y + 3, x, y + 9, c);
    gfx_line(s, x, y + 3, x + 8, y + 9, c);
    gfx_line(s, x + 8, y + 9, x + 4, y + 12, c);
}

static void draw_net_icon(struct gfx_surface *s, int x, int y, uint32_t c)
{
    for (int i = 0; i < 4; i++)                          /* four signal bars */
        gfx_fill_rect(s, x + i * 5, y + 11 - (i * 3 + 2), 3, i * 3 + 2, c);
}

static void draw_menubar(struct gfx_surface *s)
{
    gfx_fill_rect_alpha(s, 0, 0, sw, MENUBAR_H, gui_th->menu, 225);
    gfx_hline(s, 0, MENUBAR_H - 1, sw, gui_th->line);
    if (menu_open == M_APPLE)
        gfx_fill_round_rect(s, 4, 3, 36, MENUBAR_H - 6, 6, gui_th->sel);
    gfx_draw_logo(s, 14, 6, 16);
    struct gui_window *f = focused();
    const char *name = f ? f->app->name : "Finder";
    int nw = gfx_text_width(name, 1);
    name_x = 46;
    name_w = f ? nw + 12 : 0;
    if (menu_open == M_NAME)
        gfx_fill_round_rect(s, name_x, 3, name_w, MENUBAR_H - 6, 6, gui_th->sel);
    gfx_text(s, 52, 10, name, gui_th->text, GFX_TRANSPARENT, 1);      /* bold: draw twice */
    gfx_text(s, 53, 10, name, gui_th->text, GFX_TRANSPARENT, 1);
    int mxp = 52 + nw + 24;
    for (int i = 0; i < 8; i++)
        appmenu_w[i] = 0;
    if (f && f->app->menus)
        for (int i = 0; i < f->app->nmenus && i < 8; i++) {
            int tw = gfx_text_width(f->app->menus[i].title, 1);
            appmenu_x[i] = mxp - 10;
            appmenu_w[i] = tw + 20;
            if (menu_open == M_APP && dd.which == i)
                gfx_fill_round_rect(s, mxp - 10, 3, tw + 20, MENUBAR_H - 6, 6, gui_th->sel);
            gfx_text(s, mxp, 10, f->app->menus[i].title, gui_th->text, GFX_TRANSPARENT, 1);
            mxp += tw + 24;
        }
    win_w = 0;
    if (f) {                                             /* every app gets a Window menu, like macOS */
        int tw = gfx_text_width("Window", 1);
        win_x = mxp - 10;
        win_w = tw + 20;
        if (menu_open == M_WINDOW)
            gfx_fill_round_rect(s, win_x, 3, win_w, MENUBAR_H - 6, 6, gui_th->sel);
        gfx_text(s, mxp, 10, "Window", gui_th->text, GFX_TRANSPARENT, 1);
    }
    if (ptr_mode)
        gfx_text(s, sw / 2 - 150, 10, "POINTER MODE: arrows move, Enter clicks, F2/Esc exits",
                 gui_th->dim, GFX_TRANSPARENT, 1);
    uint32_t tm[6];
    char clock[24];
    sys_local_time(tm);
    if (sys_prefs.clock24)
        ksnprintf(clock, sizeof clock, "%02u:%02u", tm[3], tm[4]);
    else
        ksnprintf(clock, sizeof clock, "%u:%02u %s", tm[3] % 12 ? tm[3] % 12 : 12, tm[4], tm[3] >= 12 ? "PM" : "AM");
    int cw = gfx_text_width(clock, 1);
    gfx_text(s, sw - cw - 14, 10, clock, gui_th->text, GFX_TRANSPARENT, 1);
    clock_x = sw - cw - 22;
    /* right side, from the clock leftwards: Spotlight, network, Bluetooth, (frame rate) */
    srch_x = sw - cw - 44;
    cc_x = srch_x - 34;
    net_x = cc_x - 48;
    bt_x = net_x - 34;
    int cy = 12;
    {   /* Control Center icon: two small switches */
        if (panels_is_open())
            gfx_fill_round_rect(s, cc_x - 8, 3, 30, MENUBAR_H - 6, 6, gui_th->sel);
        gfx_fill_round_rect(s, cc_x, 6, 14, 6, 3, gui_th->text);
        gfx_fill_circle(s, cc_x + 10, 9, 2, gui_th->menu);
        gfx_fill_round_rect(s, cc_x, 14, 14, 6, 3, gui_th->dim);
        gfx_fill_circle(s, cc_x + 4, 17, 2, gui_th->menu);
    }
    gfx_fill_circle(s, srch_x, cy, 5, gui_th->text);
    gfx_fill_circle(s, srch_x, cy, 3, gui_th->menu);
    gfx_line(s, srch_x + 4, cy + 4, srch_x + 8, cy + 8, gui_th->text);
    gfx_line(s, srch_x + 5, cy + 4, srch_x + 9, cy + 8, gui_th->text);
    if (menu_open == M_STATUS)
        gfx_fill_round_rect(s, (dd.which == 0 ? net_x : bt_x) - 8, 3, dd.which == 0 ? 36 : 26, MENUBAR_H - 6, 6, gui_th->sel);
    draw_net_icon(s, net_x, 6, net_is_up() ? gui_th->text : gui_th->dim);
    draw_bt_icon(s, bt_x, 6, bt_count() && bt_enabled() ? gui_th->text : gui_th->dim);
    if (power_probe()->mobile || power_probe()->battery) {
        int bx = bt_x - 38;                              /* battery: outline only, the level cannot be read */
        gfx_rect(s, bx, 7, 22, 11, gui_th->text);
        gfx_fill_rect(s, bx + 22, 10, 2, 5, gui_th->text);
        gfx_fill_rect(s, bx + 2, 9, 18, 7, power_probe()->battery ? gui_th->text : gui_th->dim);
    }
    if (sys_prefs.airplane)
        gfx_text(s, bt_x - 26 - (power_probe()->mobile ? 40 : 0), 10, "+", gui_th->accent, GFX_TRANSPARENT, 1);
    if (sys_prefs.dnd)
        gfx_fill_circle(s, cc_x - 16, 12, 3, gui_th->accent);
    if (gui_prefs.show_fps) {
        char fp[16];
        ksnprintf(fp, sizeof fp, "%d fps", gui_fps());
        gfx_text(s, bt_x - gfx_text_width(fp, 1) - 18, 10, fp, gui_th->dim, GFX_TRANSPARENT, 1);
    }
}

/* ---- dialogs ---- */

#define DLG_W 380
static void dlg_rect(int *x, int *y, int *w, int *h)
{
    *w = DLG_W;
    *h = dlg.kind == 0 ? 150 : 130;
    *x = (sw - *w) / 2;
    *y = MAX(MENUBAR_H + 20, sh / 3 - *h / 2);
}

static void dlg_start(struct gui_window *w, int kind, const char *title, const char *msg, const char *text, int id)
{
    dlg.open = true;
    dlg.kind = kind;
    dlg.owner = win_index(w);
    dlg.id = id;
    strlcpy(dlg.title, title ? title : "", sizeof dlg.title);
    strlcpy(dlg.msg, msg ? msg : "", sizeof dlg.msg);
    strlcpy(dlg.text, text ? text : "", sizeof dlg.text);
    dd_close();
    spot_open = false;
    gui_redraw();
}

void gui_prompt(struct gui_window *w, const char *title, const char *initial, int id)
{
    dlg_start(w, 0, title, "", initial, id);
    dlg.fresh = initial && initial[0];
}

void gui_confirm(struct gui_window *w, const char *title, const char *message, int id)
{
    dlg_start(w, 1, title, message, "", id);
}

void gui_alert(struct gui_window *w, const char *title, const char *message)
{
    dlg_start(w, 2, title, message, "", -1);
}

static void dlg_finish(bool ok)
{
    struct gui_window *w = dlg.owner >= 0 ? &wins[dlg.owner] : NULL;
    int id = dlg.id;
    char text[64];

    strlcpy(text, dlg.text, sizeof text);
    dlg.open = false;
    if (w && w->used && w->app->dialog && dlg.kind != 2)
        w->app->dialog(w, id, ok, text);
    gui_redraw();
}

static void draw_dialog(struct gfx_surface *s)
{
    int x, y, w, h;

    dlg_rect(&x, &y, &w, &h);
    gfx_fill_rect_alpha(s, 0, MENUBAR_H, sw, sh - MENUBAR_H, GFX_BLACK, 70);
    gfx_fill_round_rect(s, x + 3, y + 8, w, h, 14, gfx_blend(gfx_get_pixel(s, x + 10, MIN(sh - 1, y + h + 10)), GFX_BLACK, 60));
    gfx_fill_round_rect(s, x - 1, y - 1, w + 2, h + 2, 15, gui_th->line);
    gfx_fill_round_rect(s, x, y, w, h, 14, gui_th->win);
    gfx_text(s, x + 20, y + 18, dlg.title, gui_th->text, GFX_TRANSPARENT, 2);
    if (dlg.kind == 0) {
        gui_textfield(s, x + 20, y + 58, w - 40, dlg.text, true);
    } else {
        gfx_text(s, x + 20, y + 58, dlg.msg, gui_th->dim, GFX_TRANSPARENT, 1);
    }
    int by = y + h - 40;
    if (dlg.kind != 2)
        gui_button(s, x + w - 190, by, 80, 28, "Cancel", false);
    gui_button(s, x + w - 100, by, 80, 28, "OK", true);
}

static bool dlg_click(int px, int py)
{
    int x, y, w, h;

    dlg_rect(&x, &y, &w, &h);
    int by = y + h - 40;
    if (gui_hit(px, py, x + w - 100, by, 80, 28))
        dlg_finish(true);
    else if (dlg.kind != 2 && gui_hit(px, py, x + w - 190, by, 80, 28))
        dlg_finish(false);
    return true;
}

static void dlg_key(int k)
{
    int len = (int)strlen(dlg.text);

    if (k == '\n')
        dlg_finish(true);
    else if (k == 27)
        dlg_finish(dlg.kind == 2);
    else if (dlg.kind == 0 && (k == '\b' || k == 127)) {
        if (len)
            dlg.text[len - 1] = '\0';
    } else if (dlg.kind == 0 && k >= 32 && k < 127 && len < (int)sizeof dlg.text - 2) {
        dlg.text[len] = (char)k;
        dlg.text[len + 1] = '\0';
    }
    gui_redraw();
}

/* ---- clipboard ---- */

void gui_clip_set(int kind, const char *text)
{
    clip_kind = kind;
    strlcpy(clip, text ? text : "", sizeof clip);
}
int gui_clip_kind(void) { return clip[0] ? clip_kind : GUI_CLIP_TEXT; }
const char *gui_clip_get(void) { return clip; }

int gui_mode_count(void) { return gfx_mode_count(); }
void gui_mode_at(int i, int *w, int *h) { gfx_mode_at(i, w, h); }

bool gui_set_resolution(int w, int h)
{
    if (!gfx_set_mode(w, h))
        return false;
    sw = gfx_width();
    sh = gfx_height();
    mx = MAX(0, MIN(sw - 1, mx));
    my = MAX(0, MIN(sh - 1, my));
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used) {
            if (wins[i].fullscreen) {
                win_resize(&wins[i], sw, sh);
                wins[i].x = -1;
                wins[i].y = -TITLE_H;
                continue;
            }
            if (wins[i].maximized) {
                gui_resize(&wins[i], sw - 2, sh - MENUBAR_H - TITLE_H - 2 - 84);
                wins[i].x = 0;
                wins[i].y = MENUBAR_H;
                continue;
            }
            wins[i].x = MAX(-wins[i].cw + 60, MIN(sw - 80, wins[i].x));
            wins[i].y = MAX(MENUBAR_H, MIN(sh - DOCK_RESERVE - 30, wins[i].y));
        }
    build_wallpaper();
    dd_close();
    gui_redraw();
    return true;
}

void gui_resize(struct gui_window *w, int cw, int ch)
{
    cw = MAX(160, MIN(cw, sw - 2));
    ch = MAX(100, MIN(ch, sh - MENUBAR_H - TITLE_H - 2));
    win_resize(w, cw, ch);
}

static void win_resize(struct gui_window *w, int cw, int ch)
{
    struct gfx_surface *n = gfx_surface_create(cw, ch);
    if (!n)
        return;
    gfx_surface_free(w->surf);
    w->surf = n;
    w->cw = cw;
    w->ch = ch;
    gui_redraw();
}

void gui_maximize(struct gui_window *w)
{
    if (w->fullscreen)
        return;
    if (!w->maximized) {
        w->rx = w->x;
        w->ry = w->y;
        w->rcw = w->cw;
        w->rch = w->ch;
        w->maximized = true;
        gui_resize(w, sw - 2, sh - MENUBAR_H - TITLE_H - 2 - (sys_prefs.dock_hide ? 0 : 84));
        w->x = 0;
        w->y = MENUBAR_H;
    } else {
        w->maximized = false;
        gui_resize(w, w->rcw, w->rch);
        w->x = w->rx;
        w->y = w->ry;
    }
    w->born = 0;
    gui_redraw();
}

void gui_fullscreen(struct gui_window *w)
{
    if (!w->fullscreen) {
        if (!w->maximized) {
            w->rx = w->x;
            w->ry = w->y;
            w->rcw = w->cw;
            w->rch = w->ch;
        }
        w->fullscreen = true;
        win_resize(w, sw, sh);
        w->x = -1;                                      /* the client area lands exactly on 0,0 */
        w->y = -TITLE_H;
    } else {
        w->fullscreen = false;
        if (w->maximized) {
            win_resize(w, sw - 2, sh - MENUBAR_H - TITLE_H - 2 - 84);
            w->x = 0;
            w->y = MENUBAR_H;
        } else {
            win_resize(w, w->rcw, w->rch);
            w->x = w->rx;
            w->y = w->ry;
        }
    }
    w->born = 0;
    gui_redraw();
}

void gui_minimize(struct gui_window *w)
{
    w->minimized = true;
    gui_redraw();
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
    dd_close();
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

/* The pointer is drawn on top of the finished picture and the pixels under it are remembered,
 * so moving the mouse only repaints a tiny rectangle instead of the whole desktop. */
#define CUR_W 12
#define CUR_H 19
static uint32_t cur_under[CUR_W * CUR_H];
static int cur_ux, cur_uy;
static bool cur_on;

static void cursor_paint(struct gfx_surface *s)
{
    cur_ux = mx;
    cur_uy = my;
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            cur_under[j * CUR_W + i] = (mx + i < s->w && my + j < s->h) ? gfx_get_pixel(s, mx + i, my + j) : 0;
    draw_cursor(s);
    cur_on = true;
}

static void cursor_restore(struct gfx_surface *s)
{
    if (!cur_on)
        return;
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            if (cur_ux + i < s->w && cur_uy + j < s->h)
                gfx_pixel(s, cur_ux + i, cur_uy + j, cur_under[j * CUR_W + i]);
    cur_on = false;
}

static void cursor_move_only(void)
{
    struct gfx_surface *s = gfx_screen();
    int ox = cur_ux, oy = cur_uy;

    cursor_restore(s);
    cursor_paint(s);
    gfx_flush(ox, oy, CUR_W, CUR_H);
    gfx_flush(mx, my, CUR_W, CUR_H);
}

/* A number that changes whenever something on screen reacts to the pointer position
 * (dock zoom, window button symbols, open menus). Same number = only the arrow moved. */
static uint32_t hover_sig(void)
{
    if (menu_open || spot_open || dlg.open || ptr_mode)
        return (uint32_t)(mx * 7919 + my * 104729 + 1);
    int dx, dy, dw, dh;
    if (ndock) {
        dock_rect(&dx, &dy, &dw, &dh);
        if (mx >= dx - 20 && mx < dx + dw + 20 && my >= dy - 40)
            return (uint32_t)(mx * 7919 + my * 104729 + 2);
    }
    for (int i = nz - 1; i >= 0; i--) {
        struct gui_window *w = &wins[zorder[i]];
        if (w->minimized || w->fullscreen)
            continue;
        if (mx >= w->x && mx < w->x + outer_w(w) && my >= w->y && my < w->y + outer_h(w)) {
            bool grp = mx >= w->x + 8 && mx < w->x + 72 && my >= w->y + 3 && my < w->y + 27;
            return 0x10000u + (uint32_t)zorder[i] * 2 + (grp ? 1u : 0u);
        }
    }
    return 0;
}


/* Screen magnifier: a 3x lens next to the pointer (Settings > Accessibility). */
static void draw_magnifier(struct gfx_surface *s)
{
    enum { Z = 3, LW = 270, LH = 168, SW_ = LW / Z, SH_ = LH / Z };
    static uint32_t tmp[SW_ * SH_];
    int sx = MAX(0, MIN(sw - SW_, mx - SW_ / 2)), sy = MAX(0, MIN(sh - SH_, my - SH_ / 2));
    int lx = mx + 26, ly = my + 26;

    if (!sys_prefs.acc_magnify)
        return;
    if (lx + LW > sw) lx = mx - 26 - LW;
    if (ly + LH > sh) ly = my - 26 - LH;
    lx = MAX(0, lx);
    ly = MAX(MENUBAR_H, ly);
    for (int j = 0; j < SH_; j++)
        for (int i = 0; i < SW_; i++)
            tmp[j * SW_ + i] = s->px[(sy + j) * s->stride + sx + i];
    for (int j = 0; j < LH; j++)
        for (int i = 0; i < LW; i++)
            s->px[(ly + j) * s->stride + lx + i] = tmp[(j / Z) * SW_ + i / Z];
    gfx_fill_rect(s, lx - 2, ly - 2, LW + 4, 2, gui_th->accent);
    gfx_fill_rect(s, lx - 2, ly + LH, LW + 4, 2, gui_th->accent);
    gfx_fill_rect(s, lx - 2, ly - 2, 2, LH + 4, gui_th->accent);
    gfx_fill_rect(s, lx + LW, ly - 2, 2, LH + 4, gui_th->accent);
}

/* Screen reader: a caption bar names what the pointer is over and the active window,
 * with a short tone when it changes (no speech engine). */
static void draw_reader(struct gfx_surface *s)
{
    static char last[96];
    char cap[96] = "Desktop";
    struct gui_window *front = focused();

    if (!sys_prefs.acc_reader)
        return;
    for (int i = nz - 1; i >= 0; i--) {
        struct gui_window *w = &wins[zorder[i]];
        if (!w->minimized && mx >= w->x && mx < w->x + outer_w(w) && my >= w->y && my < w->y + outer_h(w)) {
            ksnprintf(cap, sizeof cap, "Window: %s", w->title);
            break;
        }
    }
    if (my < MENUBAR_H)
        ksnprintf(cap, sizeof cap, "Menu bar");
    else if (my > sh - DOCK_RESERVE)
        ksnprintf(cap, sizeof cap, "Dock");
    if (strcmp(cap, last) != 0) {
        strlcpy(last, cap, sizeof last);
        speaker_beep(520, 15);
    }
    char line[128];
    ksnprintf(line, sizeof line, "%s   |   Active: %s", cap, front ? front->title : "none");
    int w = gfx_text_width(line, 2) + 40, x = (sw - w) / 2, y = sh - DOCK_RESERVE - 44;
    gfx_fill_round_rect(s, x, y, w, 34, 10, GFX_RGB(0, 0, 0));
    gfx_text(s, x + 20, y + 9, line, GFX_RGB(255, 255, 255), GFX_TRANSPARENT, 2);
}


/* Developer overlay (Settings > Display): every number comes from gfx_stats / display_get(), nothing is invented. */
static void draw_perf_overlay(struct gfx_surface *s)
{
    if (!gui_prefs.perf)
        return;
    const struct display_info *di = display_get();
    char l[7][48];
    int w = 236, h = 7 * 16 + 14, x = sw - w - 14, y = MENUBAR_H + 10;

    ksnprintf(l[0], 48, "FPS %u  avg %u  target %u", gfx_stats.fps, gfx_stats.fps_avg, gfx_stats.target_fps);
    ksnprintf(l[1], 48, "Frame %u.%u ms  missed %u", gfx_stats.frame_us / 1000, gfx_stats.frame_us / 100 % 10, gfx_stats.missed);
    ksnprintf(l[2], 48, "Render %u us  present %u us", gfx_stats.render_us, gfx_stats.present_us);
    ksnprintf(l[3], 48, "Dirty rows %u  pixels %u", gfx_stats.dirty_rows, gfx_stats.pixels);
    if (di->refresh_hz)
        ksnprintf(l[4], 48, "Display %d Hz (%s)", di->refresh_hz, "measured");
    else
        ksnprintf(l[4], 48, "Display refresh: unknown");
    ksnprintf(l[5], 48, "VSync: %s", di->vsync ? "hardware retrace" : "timer pacing");
    ksnprintf(l[6], 48, "Memory free %u MB", pmm_free_pages_count() / 256);
    gfx_fill_round_rect(s, x, y, w, h, 10, GFX_RGB(16, 16, 18));
    for (int i = 0; i < 7; i++)
        gfx_text(s, x + 12, y + 8 + i * 16, l[i], GFX_RGB(235, 235, 240), GFX_TRANSPARENT, 1);
}

static int fps_count, fps_last;
int gui_fps(void) { return fps_last; }

static void render(void)
{
    struct gfx_surface *s = gfx_screen();
    uint32_t render_t0 = timer_us();

    animating = false;
    if (wallpaper)
        gfx_blit(s, 0, 0, wallpaper, 0, 0, sw, sh);
    else
        gfx_fill_rect(s, 0, 0, sw, sh, gui_th->bg_top);
    if (wall_old) {                                     /* wallpaper change: the old picture fades out over 450 ms */
        int t = (int)(anim_now() - wall_old_t0);
        if (t >= 450) {
            gfx_surface_free(wall_old);
            wall_old = NULL;
        } else {
            int r = 450 - t;
            gfx_blend_surface(s, wall_old, (int)(256LL * r * r / (450 * 450)));
            animating = true;
        }
    }
    struct gui_window *front = focused();
    for (int i = 0; i < nz; i++)
        if (!wins[zorder[i]].minimized)
            draw_window(s, &wins[zorder[i]], &wins[zorder[i]] == front);
    if (!(front && front->fullscreen)) {
        draw_dock(s);
        draw_menubar(s);
    }
    if (menu_open)
        draw_menu(s);
    if (spot_open)
        draw_spotlight(s);
    panels_draw(s);
    if (snap) {                                          /* theme change: the old picture fades out over 260 ms */
        int t = (int)(timer_us() / 1000 - snap_t0);
        if (t >= 260) {
            gfx_surface_free(snap);
            snap = NULL;
        } else {
            int r = 260 - t;
            gfx_blend_surface(s, snap, (int)(256LL * r * r / (260 * 260)));
            animating = true;
        }
    }
    if (dlg.open)
        draw_dialog(s);
    draw_reader(s);
    draw_magnifier(s);
    draw_perf_overlay(s);
    cur_on = false;
    cursor_paint(s);
    uint32_t tp = timer_us();
    gfx_stats.render_us = tp - render_t0;
    if (animating)
        display_wait_vsync(25000);                      /* only when the hardware really has a retrace signal */
    gfx_stats.dirty_rows = 0;
    gfx_stats.pixels = 0;
    gfx_flush_all();
    gfx_stats.present_us = timer_us() - tp;
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
        if (!w->minimized && x >= w->x && x < w->x + outer_w(w) && y >= w->y && y < w->y + outer_h(w))
            return zorder[i];
    }
    return -1;
}

static uint32_t last_click_t;
static int last_click_x, last_click_y;

static bool fs_front(void)
{
    struct gui_window *f = focused();
    return f && f->fullscreen;
}

static void mouse_click(int x, int y, int btn)
{
    if (dlg.open) {
        dlg_click(x, y);
        return;
    }
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
        if (i == -3)                                    /* separator or dimmed item */
            return;
        bool popup = menu_open == M_POPUP;
        dd_close();
        if (y >= MENUBAR_H && !popup)
            return;
        if (popup && y >= MENUBAR_H)
            return;
    }
    if (y < MENUBAR_H && !fs_front()) {
        struct gui_window *f = focused();
        if (x < 44) {
            if (menu_open == M_NONE)
                open_apple_menu();
        } else if (f && name_w && x >= name_x && x < name_x + name_w) {
            open_name_menu(f);
        } else if (f && win_w && x >= win_x && x < win_x + win_w) {
            open_window_menu(f);
        } else if (x >= clock_x) {
            panels_toggle(PANEL_NOTIFY);
        } else if (x >= cc_x - 8 && x < cc_x + 22) {
            panels_toggle(PANEL_CONTROL);
        } else if (x >= net_x - 8 && x < net_x + 26) {
            open_status_menu(0);
        } else if (x >= bt_x - 8 && x < bt_x + 18) {
            open_status_menu(1);
        } else if (x >= srch_x - 10) {
            spot_toggle();
        } else if (f && f->app->menus) {
            for (int i = 0; i < f->app->nmenus && i < 8; i++)
                if (appmenu_w[i] && x >= appmenu_x[i] && x < appmenu_x[i] + appmenu_w[i]) {
                    open_app_menu(f, i);
                    break;
                }
        }
        return;
    }
    if (ndock && btn == GUI_BTN_LEFT && !fs_front()) {
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
    if (idx < 0) {
        int dx, dy, dw, dh;
        dock_rect(&dx, &dy, &dw, &dh);
        bool on_dock = ndock && x >= dx && x < dx + dw && y >= dy - 10 && y < dy + dh;
        if (btn == GUI_BTN_RIGHT && y >= MENUBAR_H && !fs_front() && !on_dock)
            open_desktop_menu(x, y);
        return;
    }
    struct gui_window *w = &wins[idx];
    bring_to_front(idx);
    int lx = x - w->x, ly = y - w->y;
    if (ly < TITLE_H) {
        if (btn != GUI_BTN_LEFT)
            return;
        if (lx < 70) {
            if (lx < 28)
                gui_close(w);
            else if (lx < 48)
                gui_minimize(w);
            else
                gui_maximize(w);
            return;
        }
        {   /* double click on the title bar = maximize / restore */
            uint32_t now = timer_ticks();
            if (now - last_click_t < 40 && (x - last_click_x) * (x - last_click_x) < 36) {
                last_click_t = 0;
                gui_maximize(w);
                return;
            }
            last_click_t = now;
            last_click_x = x;
            last_click_y = y;
        }
        if (!w->maximized)
            drag_idx = idx;
        drag_dx = lx;
        drag_dy = ly;
        return;
    }
    int b = btn;
    if (btn == GUI_BTN_LEFT) {                          /* double click = two clicks close together */
        uint32_t now = timer_ticks();
        if (now - last_click_t < 40 && (x - last_click_x) * (x - last_click_x) < 36 && (y - last_click_y) * (y - last_click_y) < 36) {
            b |= GUI_BTN_DOUBLE;
            last_click_t = 0;
        } else {
            last_click_t = now;
            last_click_x = x;
            last_click_y = y;
        }
    }
    if (w->app->click)
        w->app->click(w, lx - 1, ly - TITLE_H, b);
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
    bool right_now = ev->buttons & MOUSE_RIGHT, right_before = mbuttons & MOUSE_RIGHT;

    if (panels_mouse(mx, my, (uint8_t)ev->buttons, (uint8_t)mbuttons)) {
        mbuttons = ev->buttons;
        gui_request_frames();
        return;
    }
    if (ev->wheel) {                                    /* scroll wheel or touchpad edge: the window under the pointer */
        int wi = hit_window(mx, my);
        struct gui_window *tw = wi >= 0 ? &wins[wi] : focused();
        int lines = ev->wheel * 3;
        if (tw && tw->used) {
            if (tw->app->scroll)
                tw->app->scroll(tw, lines);
            else if (tw->app->key)
                for (int k = 0; k < (lines < 0 ? -lines : lines) / 3; k++)
                    tw->app->key(tw, lines < 0 ? KEY_UP : KEY_DOWN, 0);
            gui_redraw();
        }
    }
    if (left_now && !left_before)
        mouse_click(mx, my, GUI_BTN_LEFT);
    if (right_now && !right_before)
        mouse_click(mx, my, GUI_BTN_RIGHT);
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

    if (dlg.open) {
        dlg_key(k);
        return;
    }
    if (panels_key(k))
        return;
    if (ev->mods & MOD_ALT) {
        if (k == ' ') { spot_toggle(); return; }
        if (k == '\t') { cycle_windows(); return; }
        if (k == 'q' || k == 'Q') { gui_quit(); return; }
        if (k == 'm' || k == 'M') { if (menu_open) dd_close(); else open_apple_menu(); return; }
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
        case '\n':      mouse_click(mx, my, GUI_BTN_LEFT); return;
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
    if (k == KEY_F(11) && focused()) { gui_fullscreen(focused()); return; }
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
        case '\n': case ' ': mouse_click(mx, my, GUI_BTN_LEFT); return;
        case 27: ptr_mode = false; return;
        }
    }
    if (menu_open) {
        if (k == 27)
            dd_close();
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
#define KEYEV(k, m) do { ev = (struct input_event){ .type = INPUT_KEY, .key = (k), .mods = (m) }; \
                         input_push(&ev); timer_sleep_ms(300); } while (0)
    KEYEV('f', MOD_ALT);                                /* Files */
    KEYEV(14, MOD_CTRL);                                /* Ctrl+N: new folder dialog */
    for (const char *c = "docs\n"; *c; c++)
        KEYEV(*c, 0);
    KEYEV(3, MOD_CTRL);                                 /* copy */
    KEYEV(22, MOD_CTRL);                                /* paste -> "docs copy" */
    KEYEV(' ', MOD_ALT);                                /* Spotlight: Task Manager */
    for (const char *c = "task\n"; *c; c++)
        KEYEV(*c, 0);
    if (boot_has_arg("guires")) {
        gui_set_resolution(1280, 720);
        timer_sleep_ms(500);
    }
    if (boot_has_arg("guiphotos")) {                    /* camera test frame -> Pictures -> Photos viewer */
        char pth[96];
        struct camera *cam;
        struct gfx_surface *fr = gfx_surface_create(320, 240);
        camera_init();
        cam = camera_get(0);
        if (cam && fr && cam->grab) {
            cam->grab(cam, fr->px);
            ksnprintf(pth, sizeof pth, "%s/Pictures/Photo 1.bmp", users_home());
            image_save_bmp(fr, pth);
            photos_open_file(pth);
        }
        gfx_surface_free(fr);
        timer_sleep_ms(800);
    }
    if (boot_has_arg("guimax")) {
        struct gui_window *fw = focused();
        if (fw)
            gui_maximize(fw);
        timer_sleep_ms(500);
    }
    timer_sleep_ms(1200);
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
    dlg.open = false;
    dd.n = 0;
    spot_open = false;
    ptr_mode = false;
    running = true;
    mx = sw / 2;
    my = sh / 2;
    mbuttons = 0;
    drag_idx = -1;
    gui_apply_sys_prefs();
    gui_theme_apply();
    tg_light_mode = !gui_prefs.dark;
    collect_apps();
    build_dock();
    build_menu();
    gui_reload_icons();

    gui_splash();
    build_wallpaper();
    input_flush();
    display_probe();
    gui_login();
    input_flush();
    panels_init();

    gui_apply_sys_prefs();
    wall_apply_saved();
    tg_light_mode = !gui_prefs.dark;
    if (sys_prefs.open_terminal || boot_has_arg("guitest"))
        gui_open_named("Terminal");
    struct gui_window *term = (nz && sys_prefs.open_terminal) ? &wins[zorder[nz - 1]] : NULL;
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

    uint32_t last_sec = ~0u, frame_start = timer_us();
    bool was_anim = false;
    static uint32_t fps_hist[10];
    while (running) {
        struct input_event ev;
        bool redraw = need_redraw;
        int target = display_effective_fps();
        uint32_t budget = 1000000u / (uint32_t)target;
        int r;

        gfx_stats.target_fps = (uint32_t)target;
        if (animating) {
            /* Frame pacing: sleep (coarse, 10 ms tick) while there is plenty of time, then poll input until the
             * frame deadline. The 100 Hz timer is too coarse to end the wait exactly, so the last <12 ms is polled. */
            uint32_t el = timer_us() - frame_start;
            r = input_poll(&ev) ? 0 : -1;
            if (r != 0 && el < budget) {
                uint32_t rem = budget - el;
                if (rem > 14000)
                    r = input_wait(&ev, (rem - 12000) / 1000);
                while (r != 0 && (uint32_t)(timer_us() - frame_start) < budget) {
                    r = input_poll(&ev) ? 0 : -1;
                    if (r != 0)
                        __asm__ volatile ("pause");
                }
            }
        } else
            r = input_wait(&ev, 100);

        need_redraw = false;
        bool only_move = true, got_event = false, cursor_only = false;
        static uint32_t last_sig;

        while (r == 0) {
            got_event = true;
            if (ev.type == INPUT_MOUSE) {
                uint8_t b0 = mbuttons;
                handle_mouse(&ev);
                if (mbuttons != b0 || drag_idx >= 0)
                    only_move = false;
            } else {
                if (ev.type == INPUT_KEY)
                    handle_key(&ev);
                only_move = false;
            }
            r = input_poll(&ev) ? 0 : -1;
        }
        if (got_event) {
            uint32_t sg = hover_sig();
            if (!only_move || sg != last_sig || need_redraw || animating)
                redraw = true;
            else
                cursor_only = true;
            last_sig = sg;
        }
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (wins[i].used && wins[i].app->tick)
                wins[i].app->tick(&wins[i]);
        reap_windows();
        uint32_t sec = timer_uptime_sec();
        if (sec != last_sec) {
            last_sec = sec;
            fps_last = fps_count;
            fps_hist[sec % 10] = (uint32_t)fps_count;
            {
                uint32_t sum = 0;
                for (int i = 0; i < 10; i++) sum += fps_hist[i];
                gfx_stats.fps_avg = sum / 10;
            }
            gfx_stats.fps = (uint32_t)fps_count;
            fps_count = 0;
            redraw = true;
        }
        if (animating || need_redraw)
            redraw = true;
        if (redraw) {
            uint32_t t0 = timer_us();
            need_redraw = false;
            fps_count++;
            gfx_stats.frames++;
            gfx_stats.frame_us = t0 - frame_start;
            gdt_ms = (int)MAX(1, MIN(50, (int)(gfx_stats.frame_us / 1000)));
            if (was_anim && gfx_stats.frame_us > budget * 3 / 2)
                gfx_stats.missed++;
            frame_start = t0;
            render();
            was_anim = animating;
        } else if (cursor_only) {
            cursor_move_only();
        }
        if (!redraw)
            was_anim = false;
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
