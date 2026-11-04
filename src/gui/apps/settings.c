/* settings.c - "Settings": appearance, dock, pointer, display, date and time, users, storage, devices. */
#include "guiapi.h"
#include "input.h"
#include "usb.h"
#include "sys.h"
#include "blockdev.h"
#include "camera.h"
#include "vfs.h"
#include "kprintf.h"
#include "kstring.h"
#include "kerror.h"

#define SIDE_W 140

struct settings { int page; char pending[SYS_NAME_MAX]; };

static const char *pages[] = { "Appearance", "Dock", "Pointer", "Display", "Date & Time", "Users", "Storage", "Devices" };
enum { P_APP, P_DOCK, P_PTR, P_DISP, P_TIME, P_USERS, P_STORE, P_DEV, NPAGES };
enum { D_ADDUSER = 1, D_ADDPW, D_CHPW, D_DELUSER };
#define X0 (SIDE_W + 24)
#define SEGW 330

static void settings_open(struct gui_window *w)
{
    static struct settings st;

    st.page = 0;
    w->priv = &st;
}

static void heading(struct gfx_surface *s, int y, const char *text)
{
    gfx_text(s, X0, y, text, gui_th->dim, GFX_TRANSPARENT, 1);
}

static void note(struct gfx_surface *s, int y, const char *text)
{
    gfx_text(s, X0, y, text, gui_th->dim, GFX_TRANSPARENT, 1);
}

static void size_text(char *out, size_t n, uint32_t sectors)
{
    uint32_t mib = sectors / 2048;
    if (mib >= 1024)
        ksnprintf(out, n, "%u.%u GB", mib / 1024, (mib % 1024) * 10 / 1024);
    else
        ksnprintf(out, n, "%u MB", mib);
}

struct dev_ctx { struct gfx_surface *s; int y; };

static void draw_dev(struct blockdev *d, void *c)
{
    struct dev_ctx *dc = c;
    char sz[24], line[96];
    const char *mp = sys_volume_path(d->name);

    if (dc->y > 400)
        return;
    size_text(sz, sizeof sz, d->sector_count);
    ksnprintf(line, sizeof line, "%-6s %-9s %9s  %s", d->name, d->kind, sz, mp ? mp : "not mounted");
    gfx_text(dc->s, X0, dc->y, line, gui_th->text, GFX_TRANSPARENT, 1);
    dc->y += 18;
}

static void settings_draw(struct gui_window *w)
{
    struct settings *st = w->priv;
    struct gfx_surface *s = w->surf;
    char line[128];

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, gui_th->win);
    gfx_fill_rect(s, 0, 0, SIDE_W, w->ch, gui_th->side);
    gfx_vline(s, SIDE_W, 0, w->ch, gui_th->line);
    for (int i = 0; i < NPAGES; i++)
        gui_sidebar_row(s, 0, 14 + i * 28, SIDE_W, pages[i], i == st->page);

    gfx_text(s, X0, 20, pages[st->page], gui_th->text, GFX_TRANSPARENT, 2);
    switch (st->page) {
    case P_APP: {
        const char *a[] = { "Light", "Dark" }, *b[] = { "Monochrome", "Color" }, *c[] = { "On", "Off" };
        const char *sty[] = { "Colorful", "Monochrome" };
        heading(s, 64, "Theme");
        gui_segment(s, X0, 80, SEGW, a, 2, gui_prefs.dark ? 1 : 0);
        heading(s, 118, "Style");
        gui_segment(s, X0, 134, SEGW, sty, 2, gui_prefs.accent == GUI_ACCENTS - 1 ? 1 : 0);
        heading(s, 172, "Accent colour");
        for (int i = 0; i < GUI_ACCENTS; i++) {
            int cx = X0 + 15 + i * 42;
            if (i == gui_prefs.accent)
                gfx_fill_circle(s, cx, 203, 16, gui_th->text);
            gfx_fill_circle(s, cx, 203, i == gui_prefs.accent ? 13 : 15, gui_th->win);
            gfx_fill_circle(s, cx, 203, i == gui_prefs.accent ? 11 : 13, gui_accent_color(i, gui_prefs.dark));
        }
        gfx_text(s, X0, 224, gui_accent_name[gui_prefs.accent], gui_th->dim, GFX_TRANSPARENT, 1);
        heading(s, 246, "Window buttons");
        gui_segment(s, X0, 262, SEGW, b, 2, gui_prefs.color_buttons ? 1 : 0);
        heading(s, 300, "Animations");
        gui_segment(s, X0, 316, SEGW, c, 2, gui_prefs.anim ? 0 : 1);
        note(s, 360, "Tip: double-click a title bar (or the green button) to maximize.");
        break;
    }
    case P_DOCK: {
        const char *a[] = { "Colorful", "Plain" }, *b[] = { "Small", "Medium", "Large" };
        heading(s, 64, "Dock icons");
        gui_segment(s, X0, 80, SEGW, a, 2, gui_prefs.dock_colorful ? 0 : 1);
        heading(s, 118, "Icon size");
        gui_segment(s, X0, 134, SEGW, b, 3, gui_prefs.dock_size <= 44 ? 0 : gui_prefs.dock_size >= 64 ? 2 : 1);
        heading(s, 182, "Your own icon pictures");
        note(s, 200, "Put a picture named like the app in");
        if (users_current()) {
            ksnprintf(line, sizeof line, "%s/Library/Icons/", users_home());
            gfx_text(s, X0, 216, line, gui_th->text, GFX_TRANSPARENT, 1);
        }
        note(s, 234, "for example Files.png, Settings.jpg, Photos.bmp.");
        note(s, 248, "Shared icons can go in /System/Icons/.");
        gui_button(s, X0, 276, 150, 28, "Reload icons", false);
        break;
    }
    case P_PTR: {
        const char *a[] = { "Slow", "Normal", "Fast" };
        heading(s, 64, "Tracking speed");
        gui_segment(s, X0, 80, SEGW, a, 3, gui_prefs.ptr_speed);
        note(s, 126, "Tablets and touch screens follow the host pointer");
        note(s, 140, "exactly; speed applies to mice and touchpads.");
        note(s, 170, "If the pointer does not move in QEMU, click inside");
        note(s, 184, "the window (Ctrl+Alt+G releases it) or use -device usb-tablet.");
        break;
    }
    case P_DISP: {
        int nm = gui_mode_count();
        const char *hz[] = { "30 Hz", "60 Hz", "100 Hz" }, *fp[] = { "Off", "On" };
        ksnprintf(line, sizeof line, "%d x %d pixels", gui_screen_w(), gui_screen_h());
        heading(s, 54, "Resolution");
        gfx_text(s, X0, 70, line, gui_th->text, GFX_TRANSPARENT, 2);
        if (nm > 0) {
            for (int i = 0; i < nm; i++) {
                int mw, mh;
                gui_mode_at(i, &mw, &mh);
                ksnprintf(line, sizeof line, "%d x %d", mw, mh);
                gui_button(s, X0 + (i % 2) * 168, 100 + (i / 2) * 32, 160, 26, line,
                           mw == gui_screen_w() && mh == gui_screen_h());
            }
        } else {
            note(s, 108, "This computer cannot change the resolution");
            note(s, 122, "while running. Restart and open");
            note(s, 136, "\"Display resolution\" in the boot menu.");
        }
        {
            const char *f[] = { "Auto", "RGB", "BGR" };
            heading(s, 238, "Colour order (use if colours look wrong)");
            gui_segment(s, X0, 254, SEGW, f, 3, gui_prefs.pixfmt);
        }
        heading(s, 292, "Refresh rate (desktop frames per second)");
        gui_segment(s, X0, 308, SEGW, hz, 3, sys_prefs.refresh_hz <= 30 ? 0 : sys_prefs.refresh_hz >= 100 ? 2 : 1);
        heading(s, 346, "Show frame rate in the menu bar");
        gui_segment(s, X0, 362, SEGW, fp, 2, gui_prefs.show_fps ? 1 : 0);
        break;
    }
    case P_TIME: {
        uint32_t tm[6];
        const char *clk[] = { "24-hour", "12-hour" };
        sys_local_time(tm);
        ksnprintf(line, sizeof line, "%s   %02u:%02u   (UTC%+d:%02d)", sys_prefs.city, tm[3], tm[4],
                  sys_prefs.tz_minutes / 60, (sys_prefs.tz_minutes < 0 ? -sys_prefs.tz_minutes : sys_prefs.tz_minutes) % 60);
        heading(s, 54, "Location and time zone");
        gfx_text(s, X0, 70, line, gui_th->text, GFX_TRANSPARENT, 1);
        for (int i = 0; i < sys_place_count(); i++) {
            const char *city;
            int tz;
            sys_place_at(i, &city, &tz);
            gui_button(s, X0 + (i % 3) * 112, 94 + (i / 3) * 30, 104, 24, city, strcmp(city, sys_prefs.city) == 0);
        }
        heading(s, 262, "Clock");
        gui_segment(s, X0, 278, SEGW, clk, 2, sys_prefs.clock24 ? 0 : 1);
        note(s, 320, "The hardware clock is read as UTC; the time zone is added.");
        if (!sys_users_persistent())
            note(s, 336, "No MyFS disk: these settings are lost on restart.");
        break;
    }
    case P_USERS: {
        heading(s, 54, "Accounts");
        int y = 74;
        for (int i = 0; i < users_count(); i++) {
            const struct sys_user *u = users_get(i);
            ksnprintf(line, sizeof line, "%s%s%s", u->name, u->admin ? "  (admin)" : "",
                      users_current() && !strcmp(users_current()->name, u->name) ? "  - you" : "");
            gui_sidebar_row(s, X0 - 10, y, SEGW + 10, line, false);
            y += 26;
        }
        gui_button(s, X0, 290, 120, 28, "Add User...", false);
        gui_button(s, X0 + 130, 290, 170, 28, "Change Password...", false);
        ksnprintf(line, sizeof line, "Home folder: %s", users_home());
        note(s, 336, line);
        note(s, 352, sys_users_persistent() ? "Accounts are saved on the disk mounted at /Users."
                                             : "No MyFS disk found: accounts last until restart.");
        break;
    }
    case P_STORE: {
        struct dev_ctx dc = { s, 84 };
        heading(s, 54, "Disks and partitions");
        blockdev_for_each(draw_dev, &dc);
        if (dc.y == 84)
            note(s, 84, "No disks found");
        note(s, 372, "Open Files and click a disk to mount or format it.");
        break;
    }
    case P_DEV: {
        heading(s, 54, "USB devices");
        int y = 74, n = 0;
        for (int i = 0; usb_describe(i, line, sizeof line); i++) {
            gfx_text(s, X0, y, line, gui_th->text, GFX_TRANSPARENT, 1);
            y += 18;
            n++;
        }
        if (!n)
            note(s, y, "No USB devices found");
        y += 28;
        heading(s, y, "PS/2");
        gfx_text(s, X0, y + 16, mouse_present() ? "PS/2 mouse present" : "No PS/2 mouse", gui_th->text,
                 GFX_TRANSPARENT, 1);
        y += 44;
        heading(s, y, "Cameras");
        ksnprintf(line, sizeof line, "%d video source(s)", camera_count());
        gfx_text(s, X0, y + 16, line, gui_th->text, GFX_TRANSPARENT, 1);
        break;
    }
    }
}

/* index of the segment hit at x, or -1 */
static int seg_hit(int x, int y, int y0, int n)
{
    if (y < y0 || y >= y0 + 26 || x < X0 || x >= X0 + SEGW)
        return -1;
    return (x - X0) * n / SEGW;
}

static void settings_click(struct gui_window *w, int x, int y, int buttons)
{
    struct settings *st = w->priv;
    int i;
    (void)buttons;

    if (x < SIDE_W) {
        i = (y - 14) / 28;
        if (y >= 14 && i >= 0 && i < NPAGES)
            st->page = i;
        gui_redraw();
        return;
    }
    switch (st->page) {
    case P_APP:
        if ((i = seg_hit(x, y, 80, 2)) >= 0) {
            gui_set_dark(i == 1);
        } else if ((i = seg_hit(x, y, 134, 2)) >= 0) {                /* style: colourful or monochrome */
            gui_prefs.accent = i == 1 ? GUI_ACCENTS - 1 : 0;
            gui_prefs.color_buttons = i == 0;
            gui_refresh_theme();
        } else if (y >= 187 && y < 220 && x >= X0) {
            int k = (x - X0) / 42;
            if (k >= 0 && k < GUI_ACCENTS) {
                gui_prefs.accent = k;
                gui_refresh_theme();
            }
        } else if ((i = seg_hit(x, y, 262, 2)) >= 0) {
            gui_prefs.color_buttons = i == 1;
        } else if ((i = seg_hit(x, y, 316, 2)) >= 0) {
            gui_prefs.anim = i == 0;
        }
        break;
    case P_DOCK:
        if ((i = seg_hit(x, y, 80, 2)) >= 0)
            gui_prefs.dock_colorful = i == 0;
        else if ((i = seg_hit(x, y, 134, 3)) >= 0)
            gui_prefs.dock_size = i == 0 ? 44 : i == 1 ? 52 : 64;
        else if (gui_hit(x, y, X0, 276, 150, 28))
            gui_reload_icons();
        break;
    case P_PTR:
        if ((i = seg_hit(x, y, 80, 3)) >= 0)
            gui_prefs.ptr_speed = i;
        break;
    case P_DISP:
        if ((i = seg_hit(x, y, 254, 3)) >= 0) {
            gui_prefs.pixfmt = i;
            gfx_set_pixel_format(i);
        } else if ((i = seg_hit(x, y, 308, 3)) >= 0) {
            sys_prefs.refresh_hz = i == 0 ? 30 : i == 1 ? 60 : 100;
            sys_prefs_save();
        } else if ((i = seg_hit(x, y, 362, 2)) >= 0) {
            gui_prefs.show_fps = i == 1;
        } else if (y >= 100 && x >= X0) {
            int col = (x - X0) / 168, row = (y - 100) / 32;
            i = row * 2 + col;
            if (col < 2 && (y - 100) % 32 < 26 && i < gui_mode_count()) {
                int mw, mh;
                gui_mode_at(i, &mw, &mh);
                gui_set_resolution(mw, mh);
            }
        }
        break;
    case P_TIME:
        if ((i = seg_hit(x, y, 278, 2)) >= 0) {
            sys_prefs.clock24 = i == 0;
            sys_prefs_save();
        } else if (y >= 94 && x >= X0) {
            int col = (x - X0) / 112, row = (y - 94) / 30;
            i = row * 3 + col;
            if (col < 3 && (y - 94) % 30 < 24 && (x - X0) % 112 < 104 && i < sys_place_count())
                sys_place_set(i);
        }
        break;
    case P_USERS:
        if (gui_hit(x, y, X0, 290, 120, 28))
            gui_prompt(w, "New user name", "", D_ADDUSER);
        else if (gui_hit(x, y, X0 + 130, 290, 170, 28) && users_current())
            gui_prompt(w, "New password for you", "", D_CHPW);
        break;
    }
    gui_redraw();
}

static void settings_dialog(struct gui_window *w, int id, bool ok, const char *text)
{
    struct settings *st = w->priv;
    int rc;

    if (!ok)
        return;
    switch (id) {
    case D_ADDUSER:
        if (!text[0] || strlen(text) >= SYS_NAME_MAX) {
            gui_alert(w, "Users", "Please type a short name (letters and digits).");
            return;
        }
        strlcpy(st->pending, text, sizeof st->pending);
        gui_prompt(w, "Password for the new user (may be empty)", "", D_ADDPW);
        break;
    case D_ADDPW:
        rc = users_add(st->pending, text, false);
        if (rc == -EEXIST)
            gui_alert(w, "Users", "That user already exists.");
        else if (rc < 0)
            gui_alert(w, "Users", "Could not create the user (use letters and digits only).");
        break;
    case D_CHPW:
        if (users_current())
            users_set_password(users_current()->name, text);
        break;
    }
    gui_redraw();
}

static void settings_key(struct gui_window *w, int key, int mods)
{
    struct settings *st = w->priv;
    (void)mods;

    if (key == KEY_DOWN) st->page = MIN(NPAGES - 1, st->page + 1);
    else if (key == KEY_UP) st->page = MAX(0, st->page - 1);
    else if (key == 't' || key == 'T') gui_set_dark(!gui_prefs.dark);
    else if (key == 'a' || key == 'A') gui_prefs.anim = !gui_prefs.anim;
    gui_redraw();
}

static void settings_tick(struct gui_window *w)
{
    struct settings *st = w->priv;

    if (st && (st->page == P_DEV || st->page == P_TIME || st->page == P_STORE))
        gui_redraw();                                /* keep lists and the clock fresh */
}

static void settings_icon(struct gfx_surface *s, int x, int y, int sz, uint32_t g, uint32_t tile)
{
#define P(f) ((sz * (f)) / 100)
    static const int d[8][2] = { {10, 0}, {7, 7}, {0, 10}, {-7, 7}, {-10, 0}, {-7, -7}, {0, -10}, {7, -7} };
    int cx = x + sz / 2, cy = y + sz / 2;

    for (int i = 0; i < 8; i++)
        gfx_fill_circle(s, cx + d[i][0] * P(34) / 10, cy + d[i][1] * P(34) / 10, MAX(P(7), 2), g);
    gfx_fill_circle(s, cx, cy, P(27), g);
    gfx_fill_circle(s, cx, cy, P(11), tile);
#undef P
}

static const struct gui_app settings_app = {
    .name = "Settings", .title = "Settings", .cw = 580, .ch = 410, .single = true, .dock = 50,
    .open = settings_open, .draw = settings_draw, .click = settings_click, .key = settings_key,
    .tick = settings_tick, .dialog = settings_dialog, .icon = settings_icon,
};
GUI_APP(settings_app);
