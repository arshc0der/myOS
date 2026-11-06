/* settings.c - "Settings": appearance, dock, pointer, display, date and time, users, storage, devices. */
#include "guiapi.h"
#include "input.h"
#include "usb.h"
#include "sys.h"
#include "blockdev.h"
#include "camera.h"
#include "net.h"
#include "sched.h"
#include "vfs.h"
#include "kprintf.h"
#include "kstring.h"
#include "kerror.h"
#include "hw.h"
#include "display.h"
#include "version.h"
#include "pmm.h"
#include "heap.h"
#include "timer.h"
#include "multiboot.h"
#include "kernel.h"

#define SIDE_W 140

struct settings { int page; char pending[SYS_NAME_MAX]; int nap, sel_ap; struct wifi_ap ap[6]; int nbtd; struct bt_device btd[6]; };

static const char *pages[] = { "Appearance", "Dock", "Pointer", "Display", "Date & Time", "Network", "Bluetooth", "Users", "Storage", "Devices",
                               "Language", "Accessibility", "Keyboard", "Sound", "Privacy", "General", "Hardware" };
enum { P_APP, P_DOCK, P_PTR, P_DISP, P_TIME, P_NET, P_BT, P_USERS, P_STORE, P_DEV, P_LANG, P_ACC, P_KBD, P_SND, P_PRIV, P_GEN, P_HW, NPAGES };
enum { D_ADDUSER = 1, D_ADDPW, D_CHPW, D_DELUSER, D_WIFIPW, D_HOST, D_RESET_SET, D_RESET_ALL };

static char upd_msg[160];
static volatile int ping_ms = -3;                 /* -3 never run, -2 running, -1 no answer, >=0 round trip */

static void ping_task(void *arg)
{
    struct netdev *d = net_default();
    (void)arg;
    ping_ms = (d && d->gw) ? net_ping(d->gw, 2000) : -1;
}
#define X0 (SIDE_W + 24)
#define SEGW 330

static struct settings st_store;

static void settings_open(struct gui_window *w)
{
    st_store.page = 0;
    if (sys_prefs.check_updates && !upd_msg[0])
        sys_update_check(upd_msg, sizeof upd_msg);
    w->priv = &st_store;
}

void settings_show(const char *page)
{
    struct gui_window *w = gui_open_named("Settings");
    if (!w)
        return;
    for (int i = 0; i < NPAGES; i++)
        if (!strcmp(pages[i], page))
            st_store.page = i;
    gui_redraw();
}

static void heading(struct gfx_surface *s, int y, const char *text)
{
    gfx_text(s, X0, y, text, gui_th->dim, GFX_TRANSPARENT, 1);
}

static void note(struct gfx_surface *s, int y, const char *text)
{
    gfx_text(s, X0, y, text, gui_th->dim, GFX_TRANSPARENT, 1);
}


/* toggle row: label, description, switch on the right */
static void trow(struct gfx_surface *s, int y, const char *label, const char *desc, bool on)
{
    gfx_text(s, X0, y + 2, label, gui_th->text, GFX_TRANSPARENT, 1);
    gfx_text(s, X0, y + 18, desc, gui_th->dim, GFX_TRANSPARENT, 1);
    gui_switch(s, X0 + SEGW + 20, y + 5, on);
}
static bool trow_hit(int x, int y, int y0) { return gui_hit(x, y, X0, y0 - 2, SEGW + 70, 38); }

/* 3-column grid of buttons; returns index at (x,y) or -1 */
static void grid_draw(struct gfx_surface *s, int y0, const char *const *names, int n, int sel)
{
    for (int i = 0; i < n; i++)
        gui_button(s, X0 + (i % 3) * 138, y0 + (i / 3) * 30, 130, 24, names[i], i == sel);
}
static int grid_hit(int x, int y, int y0, int n)
{
    int c = (x - X0) / 138, r = (y - y0) / 30;
    if (x < X0 || y < y0 || c > 2 || (x - X0) % 138 > 130 || (y - y0) % 30 > 24)
        return -1;
    return r * 3 + c < n ? r * 3 + c : -1;
}

static bool *pref_toggle(int page, int i)
{
    switch (page) {
    case P_ACC: { bool *p[5] = { &sys_prefs.acc_reader, &sys_prefs.acc_magnify, &sys_prefs.acc_contrast, &sys_prefs.acc_kbd_pointer, &sys_prefs.acc_reduce_motion }; return i < 5 ? p[i] : NULL; }
    case P_PRIV: { bool *p[4] = { &sys_prefs.priv_diag, &sys_prefs.priv_location, &sys_prefs.priv_analytics, &sys_prefs.require_password }; return i < 4 ? p[i] : NULL; }
    case P_GEN: { bool *p[4] = { &sys_prefs.auto_updates, &sys_prefs.check_updates, &sys_prefs.voice, &sys_prefs.open_terminal }; return i < 4 ? p[i] : NULL; }
    }
    return NULL;
}

static void pref_apply(void)
{
    gui_prefs.accent = sys_prefs.acc_contrast ? GUI_ACCENTS - 1 : sys_prefs.accent;
    gui_prefs.anim = !sys_prefs.acc_reduce_motion;
    sys_prefs_save();
    gui_refresh_theme();
}

static const char *const lay_names[5] = { "US English", "UK English", "German", "French", "Hindi" };

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
        gui_sidebar_row(s, 0, 14 + i * 28, SIDE_W, sys_tr(pages[i]), i == st->page);

    gfx_text(s, X0, 20, sys_tr(pages[st->page]), gui_th->text, GFX_TRANSPARENT, 2);
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
        const char *hz[] = { "Auto", "30", "60", "90", "120" }, *fp[] = { "Off", "On" };
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
        heading(s, 292, "Target frame rate (frames per second)");
        gui_segment(s, X0, 308, SEGW, hz, 5, sys_prefs.refresh_hz == 0 ? 0 : sys_prefs.refresh_hz <= 30 ? 1 : sys_prefs.refresh_hz <= 60 ? 2 : sys_prefs.refresh_hz <= 90 ? 3 : 4);
        heading(s, 346, "Show frame rate in the menu bar");
        gui_segment(s, X0, 362, SEGW, fp, 2, gui_prefs.show_fps ? 1 : 0);
        heading(s, 396, "Performance overlay (developer)");
        gui_segment(s, X0, 412, SEGW, fp, 2, gui_prefs.perf ? 1 : 0);
        {
            const struct display_info *di = display_get();
            char l[80];
            if (di->refresh_hz)
                ksnprintf(l, sizeof l, "Monitor: %d Hz, %s. Target %d FPS.", di->refresh_hz, di->refresh_src, display_effective_fps());
            else
                ksnprintf(l, sizeof l, "Monitor refresh: unknown. Target %d FPS.", display_effective_fps());
            note(s, 450, l);
            ksnprintf(l, sizeof l, "Measured %u FPS. Sync: %s.", gfx_stats.fps, di->vsync ? "hardware retrace" : "timer pacing");
            note(s, 466, l);
            note(s, 482, di->refresh_hz ? "Targets above the monitor rate are ignored." : "No signal to read the rate: Auto uses 60.");
        }
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
    case P_NET: {
        struct netdev *nd = net_default();
        heading(s, 54, "Ethernet");
        gfx_text(s, X0, 70, net_status_text(), gui_th->text, GFX_TRANSPARENT, 1);
        if (nd) {
            char a[20], b[20], c[20];
            ksnprintf(line, sizeof line, "Card      %s  %02x:%02x:%02x:%02x:%02x:%02x  link %s", nd->driver, nd->mac[0], nd->mac[1],
                      nd->mac[2], nd->mac[3], nd->mac[4], nd->mac[5], nd->link ? "up" : "down");
            gfx_text(s, X0, 92, line, gui_th->dim, GFX_TRANSPARENT, 1);
            net_format_ip(nd->ip, a, sizeof a);
            net_format_ip(nd->mask, b, sizeof b);
            ksnprintf(line, sizeof line, "Address   %s   mask %s", a, b);
            gfx_text(s, X0, 110, line, gui_th->dim, GFX_TRANSPARENT, 1);
            net_format_ip(nd->gw, a, sizeof a);
            net_format_ip(nd->dns, c, sizeof c);
            ksnprintf(line, sizeof line, "Gateway   %s   DNS %s", a, c);
            gfx_text(s, X0, 128, line, gui_th->dim, GFX_TRANSPARENT, 1);
            ksnprintf(line, sizeof line, "Frames    %u received, %u sent", nd->rx_frames, nd->tx_frames);
            gfx_text(s, X0, 146, line, gui_th->dim, GFX_TRANSPARENT, 1);
        } else {
            note(s, 92, "Supported cards: e1000 (QEMU default), virtio-net (UTM).");
            note(s, 108, "Start QEMU with  -nic user,model=e1000  if none shows up.");
        }
        gui_button(s, X0, 170, 160, 28, net_busy() ? "Connecting..." : "Connect (DHCP)", false);
        gui_button(s, X0 + 170, 170, 160, 28, "Ping gateway", false);
        if (ping_ms == -2) note(s, 206, "Ping: waiting...");
        else if (ping_ms >= 0) { ksnprintf(line, sizeof line, "Ping: reply in %d ms", ping_ms); note(s, 206, line); }
        else if (ping_ms == -1) note(s, 206, "Ping: no answer");
        heading(s, 240, "Wi-Fi");
        if (wifi_count()) {
            struct wifi_adapter *wa = wifi_get(0);
            ksnprintf(line, sizeof line, "%s: %s", wa->name, wa->status ? wa->status(wa) : "ready");
            gfx_text(s, X0, 258, line, gui_th->text, GFX_TRANSPARENT, 1);
            gui_button(s, X0, 280, 120, 26, "Scan", false);
            for (int i = 0; i < st->nap; i++) {
                ksnprintf(line, sizeof line, "%s%s   signal %d%%", st->ap[i].ssid, st->ap[i].secured ? " (secured)" : "", st->ap[i].signal);
                gui_sidebar_row(s, X0 - 10, 312 + i * 24, SEGW + 10, line, false);
            }
        } else {
            char hw[4][40];
            int nh = wifi_detect_hardware(hw, 4);
            note(s, 258, "No Wi-Fi driver in MyOS (radios need vendor firmware).");
            for (int i = 0; i < nh; i++) {
                ksnprintf(line, sizeof line, "Found radio: %s", hw[i]);
                gfx_text(s, X0, 276 + i * 16, line, gui_th->text, GFX_TRANSPARENT, 1);
            }
            if (!nh)
                note(s, 276, "No Wi-Fi radio found (virtual machines use the wired card).");
            note(s, 344, "Add a driver: see docs/NETWORK.md (wifi_register).");
        }
        break;
    }
    case P_BT: {
        heading(s, 54, "Bluetooth");
        if (bt_count()) {
            gfx_text(s, X0, 70, bt_get(0)->name, gui_th->text, GFX_TRANSPARENT, 1);
            gui_button(s, X0, 94, 120, 26, "Scan", false);
            for (int i = 0; i < st->nbtd; i++) {
                ksnprintf(line, sizeof line, "%s%s", st->btd[i].name, st->btd[i].connected ? "   connected" : st->btd[i].paired ? "   paired" : "");
                gui_sidebar_row(s, X0 - 10, 130 + i * 24, SEGW + 10, line, false);
            }
        } else {
            char hw[4][40];
            int nh = bt_detect_hardware(hw, 4);
            note(s, 70, "No Bluetooth driver in MyOS.");
            note(s, 86, "A driver needs the HCI protocol over USB bulk transfers,");
            note(s, 102, "which the USB stack does not have yet.");
            for (int i = 0; i < nh; i++) {
                ksnprintf(line, sizeof line, "Found adapter: %s", hw[i]);
                gfx_text(s, X0, 130 + i * 16, line, gui_th->text, GFX_TRANSPARENT, 1);
            }
            if (!nh)
                note(s, 130, "No Bluetooth adapter found.");
            note(s, 200, "Add a driver: see docs/NETWORK.md (bt_register).");
        }
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
    case P_LANG: {
        char l[80];
        heading(s, 54, "Language");
        grid_draw(s, 70, sys_languages, sys_language_count(), sys_prefs.language);
        heading(s, 168, "Country or region");
        grid_draw(s, 184, sys_countries, sys_country_count(), sys_prefs.country);
        ksnprintf(l, sizeof l, "Time zone %s   Date %s   Clock %s", sys_prefs.city,
                  sys_prefs.date_format == 0 ? "DD/MM/YYYY" : sys_prefs.date_format == 1 ? "MM/DD/YYYY" : "YYYY-MM-DD",
                  sys_prefs.clock24 ? "24-hour" : "12-hour");
        gfx_text(s, X0, 316, l, gui_th->text, GFX_TRANSPARENT, 1);
        note(s, 336, "Language is saved. Menus are English in this version.");
        note(s, 352, "Time zone, date format and clock can be changed on Date & Time.");
        break;
    }
    case P_ACC: {
        static const char *const lab[5] = { "Screen reader", "Screen magnification", "High contrast", "Keyboard navigation", "Reduce motion" };
        static const char *const dsc[5] = { "Captions + tone for the window under the pointer", "3x lens next to the pointer", "Black and white interface (works now)",
            "Move the pointer with Alt + arrow keys", "Turn off animations (works now)" };
        for (int i = 0; i < 5; i++)
            trow(s, 60 + i * 46, lab[i], dsc[i], *pref_toggle(P_ACC, i));
        break;
    }
    case P_KBD: {
        heading(s, 54, "Keyboard layout");
        grid_draw(s, 70, lay_names, 5, sys_prefs.kbd_layout);
        note(s, 140, "The layout is saved. Key mapping is US English for now.");
        gfx_text(s, X0, 176, "Detected: keyboard (PS/2 or USB HID)", gui_th->text, GFX_TRANSPARENT, 1);
        note(s, 198, "Pointer: see the Pointer page. Mouse present:");
        gfx_text(s, X0 + 330, 198, mouse_present() ? "yes" : "no", gui_th->text, GFX_TRANSPARENT, 1);
        break;
    }
    case P_SND: {
        char l[40];
        heading(s, 54, "Output volume");
        gui_slider(s, X0, 78, SEGW, sys_prefs.muted ? 0 : sys_prefs.volume, 100);
        ksnprintf(l, sizeof l, "%d%%", sys_prefs.muted ? 0 : sys_prefs.volume);
        gfx_text(s, X0 + SEGW + 12, 76, l, gui_th->text, GFX_TRANSPARENT, 1);
        trow(s, 110, "Mute", "Silence all sound", sys_prefs.muted);
        note(s, 170, "MyOS has no audio driver yet: volume and mute are saved only.");
        break;
    }
    case P_PRIV: {
        static const char *const lab[4] = { "Share diagnostic data", "Location services", "Share usage analytics", "Require password at login" };
        static const char *const dsc[4] = { "Nothing is sent: no network client yet", "Time zone based location for apps", "Anonymous usage statistics", "Off = log in with one click" };
        for (int i = 0; i < 4; i++)
            trow(s, 60 + i * 46, lab[i], dsc[i], *pref_toggle(P_PRIV, i));
        note(s, 262, "Passwords are stored as salted hashes (teaching grade).");
        break;
    }
    case P_GEN: {
        static const char *const lab[4] = { "Install updates automatically", "Check for updates", "Voice assistant", "Open Terminal after login" };
        static const char *const dsc[4] = { "Look on disks (MyOSUpdate) at startup", "When connected to a network", "Assistant app answers with a tone (typed)", "Terminal window at startup" };
        char l[96];
        heading(s, 54, "Computer name");
        ksnprintf(l, sizeof l, "%s   (%s.local)", sys_prefs.hostname, sys_prefs.hostname);
        gfx_text(s, X0, 72, l, gui_th->text, GFX_TRANSPARENT, 1);
        gui_button(s, X0 + SEGW - 100, 66, 100, 24, "Rename", false);
        for (int i = 0; i < 4; i++)
            trow(s, 100 + i * 42, lab[i], dsc[i], *pref_toggle(P_GEN, i));
        heading(s, 272, "Startup");
        ksnprintf(l, sizeof l, "Boot loader: %s", boot_info.loader[0] ? boot_info.loader : "unknown");
        gfx_text(s, X0, 288, l, gui_th->text, GFX_TRANSPARENT, 1);
        ksnprintf(l, sizeof l, "Boot options: %.60s", boot_info.cmdline[0] ? boot_info.cmdline : "(none)");
        gfx_text(s, X0, 304, l, gui_th->dim, GFX_TRANSPARENT, 1);
        note(s, 320, "Edit boot options in grub.cfg (for example nosetup skips setup).");
        heading(s, 350, "Factory reset");
        gui_button(s, X0, 368, 190, 28, "Reset settings...", false);
        gui_button(s, X0 + 200, 368, 220, 28, "Erase everything...", false);
        note(s, 404, "Both restart the computer and run the Setup Assistant again.");
        heading(s, 428, "Software update");
        gui_button(s, X0, 444, 140, 26, "Check now", false);
        if (upd_msg[0]) {
            char a[64];
            strlcpy(a, upd_msg, sizeof a);
            note(s, 476, a);
            if (strlen(upd_msg) > 56) note(s, 490, upd_msg + 56);
        }
        break;
    }
    case P_HW: {
        char l[96], brand[49];
        struct heap_stats hs;
        struct pci_device pd[64];
        uint32_t up = timer_uptime_sec();
        heap_get_stats(&hs);
        if (!cpu_brand(brand)) strlcpy(brand, "x86 processor", sizeof brand);
        heading(s, 54, "Processor");
        gfx_text(s, X0, 70, brand, gui_th->text, GFX_TRANSPARENT, 1);
        heading(s, 100, "Memory");
        ksnprintf(l, sizeof l, "%u MB installed", pmm_total_pages() / 256);
        gfx_text(s, X0, 116, l, gui_th->text, GFX_TRANSPARENT, 1);
        ksnprintf(l, sizeof l, "Kernel heap: %u KB used of %u KB, %u allocations", hs.used_bytes / 1024, hs.total_bytes / 1024, hs.alloc_count);
        gfx_text(s, X0, 134, l, gui_th->dim, GFX_TRANSPARENT, 1);
        heading(s, 164, "Display");
        if (display_get()->refresh_hz)
            ksnprintf(l, sizeof l, "%d x %d, %d Hz (measured)", gui_screen_w(), gui_screen_h(), display_get()->refresh_hz);
        else
            ksnprintf(l, sizeof l, "%d x %d, refresh rate unknown", gui_screen_w(), gui_screen_h());
        gfx_text(s, X0, 180, l, gui_th->text, GFX_TRANSPARENT, 1);
        heading(s, 210, "Devices");
        ksnprintf(l, sizeof l, "%d PCI devices, %d disks, %d network adapters", pci_scan(pd, 64), blockdev_count(), net_probe());
        gfx_text(s, X0, 226, l, gui_th->text, GFX_TRANSPARENT, 1);
        heading(s, 256, "Uptime");
        ksnprintf(l, sizeof l, "%u h %u min %u s", up / 3600, up / 60 % 60, up % 60);
        gfx_text(s, X0, 272, l, gui_th->text, GFX_TRANSPARENT, 1);
        heading(s, 302, "System");
        ksnprintf(l, sizeof l, "MyOS %s   computer name %s", MYOS_VERSION, sys_prefs.hostname);
        gfx_text(s, X0, 318, l, gui_th->text, GFX_TRANSPARENT, 1);
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
            gui_prefs.accent = i == 1 ? GUI_ACCENTS - 1 : 0; sys_prefs.accent = gui_prefs.accent; sys_prefs_save();
            gui_prefs.color_buttons = i == 0;
            gui_refresh_theme();
        } else if (y >= 187 && y < 220 && x >= X0) {
            int k = (x - X0) / 42;
            if (k >= 0 && k < GUI_ACCENTS) {
                { gui_prefs.accent = k; sys_prefs.accent = k; sys_prefs_save(); }
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
        } else if ((i = seg_hit(x, y, 308, 5)) >= 0) {
            static const int tf[5] = { 0, 30, 60, 90, 120 };
            if (!display_get()->refresh_hz || tf[i] <= display_get()->refresh_hz) {   /* only what the display can show */
                sys_prefs.refresh_hz = tf[i];
                sys_prefs_save();
            }
        } else if ((i = seg_hit(x, y, 412, 2)) >= 0) {
            gui_prefs.perf = i == 1;
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
    case P_NET:
        if (gui_hit(x, y, X0, 170, 160, 28))
            net_start_async();
        else if (gui_hit(x, y, X0 + 170, 170, 160, 28) && net_is_up()) {
            ping_ms = -2;
            task_create("ping", ping_task, NULL);
        } else if (wifi_count() && gui_hit(x, y, X0, 280, 120, 26)) {
            struct wifi_adapter *wa = wifi_get(0);
            st->nap = wa->scan ? wa->scan(wa, st->ap, 6) : 0;
            if (st->nap < 0) st->nap = 0;
        } else if (wifi_count() && y >= 312 && x >= X0 - 10) {
            int k = (y - 312) / 24;
            if (k < st->nap) {
                st->sel_ap = k;
                if (st->ap[k].secured)
                    gui_prompt(w, "Wi-Fi password", "", D_WIFIPW);
                else
                    wifi_get(0)->connect(wifi_get(0), st->ap[k].ssid, "");
            }
        }
        break;
    case P_BT:
        if (bt_count() && gui_hit(x, y, X0, 94, 120, 26)) {
            struct bt_adapter *ba = bt_get(0);
            st->nbtd = ba->scan ? ba->scan(ba, st->btd, 6) : 0;
            if (st->nbtd < 0) st->nbtd = 0;
        } else if (bt_count() && y >= 130 && x >= X0 - 10) {
            int k = (y - 130) / 24;
            struct bt_adapter *ba = bt_get(0);
            if (k < st->nbtd) {
                if (!st->btd[k].paired && ba->pair) ba->pair(ba, st->btd[k].addr);
                else if (ba->connect) ba->connect(ba, st->btd[k].addr);
            }
        }
        break;
    case P_USERS:
        if (gui_hit(x, y, X0, 290, 120, 28))
            gui_prompt(w, "New user name", "", D_ADDUSER);
        else if (gui_hit(x, y, X0 + 130, 290, 170, 28) && users_current())
            gui_prompt(w, "New password for you", "", D_CHPW);
        break;
    default:
        break;
    }
    switch (st->page) {
    case P_LANG:
        if ((i = grid_hit(x, y, 70, sys_language_count())) >= 0) { sys_prefs.language = i; sys_prefs_save(); }
        else if ((i = grid_hit(x, y, 184, sys_country_count())) >= 0) { sys_country_apply(i); sys_prefs_save(); }
        break;
    case P_ACC:
    case P_PRIV:
        for (i = 0; i < (st->page == P_ACC ? 5 : 4); i++)
            if (trow_hit(x, y, 60 + i * 46)) { bool *p = pref_toggle(st->page, i); *p = !*p; pref_apply(); }
        break;
    case P_KBD:
        if ((i = grid_hit(x, y, 70, 5)) >= 0) { sys_prefs.kbd_layout = i; sys_prefs_save(); }
        break;
    case P_SND:
        if (gui_hit(x, y, X0, 74, SEGW, 24)) { sys_prefs.volume = gui_slider_value(x, X0, SEGW, 100); sys_prefs.muted = false; sys_prefs_save(); }
        else if (trow_hit(x, y, 110)) { sys_prefs.muted = !sys_prefs.muted; sys_prefs_save(); }
        break;
    case P_GEN:
        if (gui_hit(x, y, X0 + SEGW - 100, 66, 100, 24))
            gui_prompt(w, "Computer name", sys_prefs.hostname, D_HOST);
        else if (gui_hit(x, y, X0, 444, 140, 26))
            sys_update_check(upd_msg, sizeof upd_msg);
        else if (gui_hit(x, y, X0, 368, 190, 28))
            gui_confirm(w, "Reset settings", "Restore default settings and run the Setup Assistant again? Accounts and files are kept. The computer restarts.", D_RESET_SET);
        else if (gui_hit(x, y, X0 + 200, 368, 220, 28))
            gui_confirm(w, "Erase everything", "Delete all accounts, home folders and settings, then restart into the Setup Assistant? This cannot be undone.", D_RESET_ALL);
        else
            for (i = 0; i < 4; i++)
                if (trow_hit(x, y, 100 + i * 42)) { bool *p = pref_toggle(P_GEN, i); *p = !*p; pref_apply(); }
        break;
    default:
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
    case D_HOST:
        if (sys_hostname_ok(text)) { strlcpy(sys_prefs.hostname, text, sizeof sys_prefs.hostname); sys_prefs_save(); }
        else gui_alert(w, "Computer name", "Use letters, digits and - only (23 characters at most).");
        break;
    case D_RESET_SET:
        sys_factory_reset(false);
        system_reboot();
        break;
    case D_RESET_ALL:
        sys_factory_reset(true);
        system_reboot();
        break;
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
    case D_WIFIPW:
        if (wifi_count() && st->sel_ap < st->nap)
            wifi_get(0)->connect(wifi_get(0), st->ap[st->sel_ap].ssid, text);
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

    if (st && (st->page == P_HW || st->page == P_DEV || st->page == P_TIME || st->page == P_STORE || st->page == P_NET))
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
    .name = "Settings", .title = "Settings", .cw = 600, .ch = 520, .single = true, .dock = 50,
    .open = settings_open, .draw = settings_draw, .click = settings_click, .key = settings_key,
    .tick = settings_tick, .dialog = settings_dialog, .icon = settings_icon,
};
GUI_APP(settings_app);
