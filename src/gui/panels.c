/*
 * panels.c - Control Center and Notification Center, in the style of macOS.
 *
 * Control Center (menu bar icon): Wi-Fi, Bluetooth, Airplane Mode, Ethernet, Do Not Disturb, Dark/Light mode,
 * Display brightness slider, Sound volume slider, battery line and shortcuts to System Settings.
 * Notification Center (click the clock): notifications, Do Not Disturb, month calendar with events.
 *
 * Honest notes: brightness dims the frame buffer in software (no backlight driver); sound volume is stored and
 * used by the PC speaker only (no audio driver); Wi-Fi has no driver, so its tile says so; the battery level
 * cannot be read (see powerinfo.c). Everything fades and slides with time-based easing.
 */
#include "panels.h"
#include "guiapi.h"
#include "sys.h"
#include "net.h"
#include "notify.h"
#include "timer.h"
#include "uifont.h"
#include "kstring.h"
#include "kprintf.h"
#include "input.h"

#define CCW 340
#define NCW 360
#define MAXH 620

static int which_open = PANEL_NONE;
static uint32_t t_change;                       /* ms: when it started opening or closing */
static bool closing;
static uint32_t closed_ms;
static int closed_which;
static struct gfx_surface *ps;                  /* the panel is drawn here, then blended onto the screen */

/* hit rectangles in panel coordinates, rebuilt on every draw */
enum { H_WIFI = 1, H_BT, H_AIR, H_ETH, H_DND, H_DARK, H_BRIGHT, H_VOL, H_MUTE, H_SET_NET, H_SET_SND,
       H_CLEAR, H_DNDROW, H_PREV, H_NEXT, H_ADD, H_DAY = 100, H_NOTI = 200, H_EV = 300 };
static struct { int x, y, w, h, id; } hits[96];
static int nh;
static int panel_x, panel_y, panel_w, panel_h;

/* slider easing */
static int drag;                                /* H_BRIGHT / H_VOL while the pointer drags */
static int vol_disp = -1, bri_disp = -1;        /* what the knobs show (eases toward the real value) */
static int bri_x10 = -1;                        /* brightness really applied to the screen, x10 for smooth steps */

/* calendar view */
static int vy, vm, sel_d, today_y, today_m, today_d;
static bool adding;
static char addbuf[40];

/* toast banner */
static uint32_t seen_serial, toast_t0;
static struct notification toast;
static bool toast_on;

static uint32_t now_ms(void) { return timer_us() / 1000; }

static void hit(int x, int y, int w, int h, int id)
{
    if (nh < 96) {
        hits[nh].x = x; hits[nh].y = y; hits[nh].w = w; hits[nh].h = h; hits[nh].id = id;
        nh++;
    }
}

static int hit_at(int x, int y)
{
    for (int i = nh - 1; i >= 0; i--)
        if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h)
            return hits[i].id;
    return 0;
}

/* ease-out cubic, t and d in ms, returns 0..256 */
static int ease(int t, int d)
{
    if (t <= 0) return 0;
    if (t >= d) return 256;
    int r = d - t;
    return 256 - (int)(256LL * r * r * r / ((long long)d * d * d));
}

void panels_apply_radios(void)
{
    wifi_set_enabled(!sys_prefs.airplane && sys_prefs.wifi_on);
    bt_set_enabled(!sys_prefs.airplane && sys_prefs.bt_on);
}

static void load_today(void)
{
    uint32_t t[6];
    sys_local_time(t);
    today_y = (int)t[0]; today_m = (int)t[1]; today_d = (int)t[2];
    vy = today_y; vm = today_m; sel_d = today_d;
}

void panels_init(void)
{
    char b[80];
    int idx[4];

    panels_apply_radios();
    bri_x10 = sys_prefs.brightness * 10;
    gfx_set_brightness(sys_prefs.brightness);
    power_probe();
    load_today();
    cal_load();
    seen_serial = notify_serial;
    ksnprintf(b, sizeof b, "Signed in as %s", users_current() ? users_current()->name : "user");
    notify_post("MyOS", "Welcome", b);
    int n = cal_on_day(today_y, today_m, today_d, idx, 4);
    for (int i = 0; i < n; i++)
        notify_post("Calendar", "Today", cal_get(idx[i])->text);
}

bool panels_is_open(void) { return which_open != PANEL_NONE; }

void panels_toggle(int which)
{
    if (which == closed_which && now_ms() - closed_ms < 80)
        return;                                 /* the click that closed it must not reopen it */
    if (which_open == which && !closing) {
        closing = true;
        t_change = now_ms();
        return;
    }
    which_open = which;
    closing = false;
    t_change = now_ms();
    adding = false;
    load_today();
    vol_disp = sys_prefs.volume;
    bri_disp = sys_prefs.brightness;
    gui_request_frames();
}

static void close_now(void)
{
    if (which_open == PANEL_NONE)
        return;
    closed_which = which_open;
    closed_ms = now_ms();
    closing = true;
    t_change = now_ms();
    gui_request_frames();
}

/* ------------------------------------------------------------------ */
/* Drawing helpers: coordinates are relative to the panel origin       */
/* ------------------------------------------------------------------ */

static int ox, oy;

static void label(int x, int y, const char *t, uint32_t c, int px, int weight)
{
    ui_text(ps, ox + x, oy + y, t, c, px, weight);
}

static void rrect(int x, int y, int w, int h, int r, uint32_t c) { gfx_fill_round_rect(ps, ox + x, oy + y, w, h, r, c); }
static void disc(int x, int y, int r, uint32_t c) { gfx_fill_circle(ps, ox + x, oy + y, r, c); }
static void ln(int x0, int y0, int x1, int y1, uint32_t c) { gfx_line(ps, ox + x0, oy + y0, ox + x1, oy + y1, c); }

static void tile(int x, int y, int w, int h, const char *title, const char *sub, bool on, int id, int glyph)
{
    rrect(x, y, w, h, 14, on ? gfx_blend(gui_th->win, gui_th->accent, 40) : gui_th->side);
    uint32_t ic = on ? gui_th->accent : gfx_blend(gui_th->side, gui_th->text, 90);
    uint32_t g = on ? gui_th->accent_fg : gui_th->win;
    int cx = x + 28, cy = y + h / 2;
    disc(cx, cy, 17, ic);
    switch (glyph) {
    case 0:                                         /* Wi-Fi: dot and three arcs */
        disc(cx, cy + 7, 2, g);
        for (int r = 6; r <= 14; r += 4)
            for (int a = -45; a <= 45; a += 5) {
                int dx = a * r / 50, dy = -(r * (100 - a * a / 40)) / 100;
                gfx_pixel(ps, ox + cx + dx, oy + cy + 7 + dy, g);
                gfx_pixel(ps, ox + cx + dx, oy + cy + 8 + dy, g);
            }
        break;
    case 1:                                         /* Bluetooth rune */
        ln(cx, cy - 9, cx, cy + 9, g);
        ln(cx, cy - 9, cx + 6, cy - 3, g);
        ln(cx + 6, cy - 3, cx - 6, cy + 5, g);
        ln(cx, cy + 9, cx + 6, cy + 3, g);
        ln(cx + 6, cy + 3, cx - 6, cy - 5, g);
        break;
    case 2:                                         /* aeroplane */
        gfx_fill_rect(ps, ox + cx - 8, oy + cy - 1, 17, 3, g);
        ln(cx, cy - 8, cx + 3, cy, g);
        ln(cx, cy + 8, cx + 3, cy + 1, g);
        ln(cx - 7, cy - 4, cx - 4, cy, g);
        break;
    case 3:                                         /* Ethernet plug */
        gfx_fill_rect(ps, ox + cx - 6, oy + cy - 6, 12, 10, g);
        gfx_fill_rect(ps, ox + cx - 2, oy + cy + 4, 4, 5, g);
        break;
    case 4:                                         /* moon */
        disc(cx, cy, 8, g);
        disc(cx + 4, cy - 3, 7, ic);
        break;
    default:                                        /* half disc */
        disc(cx, cy, 8, g);
        gfx_fill_rect(ps, ox + cx, oy + cy - 9, 10, 18, ic);
        disc(cx, cy, 3, g);
        break;
    }
    label(x + 54, y + h / 2 - 18, title, gui_th->text, 14, UI_SEMIBOLD);
    label(x + 54, y + h / 2 + 2, sub, gui_th->dim, 12, UI_REGULAR);
    hit(x, y, w, h, id);
}

static void slider(int x, int y, int w, int value, int lo, int id, const char *title, const char *right)
{
    label(x, y, title, gui_th->text, 14, UI_SEMIBOLD);
    label(x + w - ui_text_width(right, 12, UI_REGULAR), y + 2, right, gui_th->dim, 12, UI_REGULAR);
    int ty = y + 24, h = 22;
    int fill = (value - lo) * (w - h) / (100 - lo) + h;
    fill = MAX(h, MIN(w, fill));
    rrect(x, ty, w, h, h / 2, gui_th->line);
    rrect(x, ty, fill, h, h / 2, gui_th->accent);
    disc(x + fill - h / 2, ty + h / 2, h / 2 - 3, GFX_RGB(255, 255, 255));
    hit(x, ty - 4, w, h + 8, id);
}

static void button(int x, int y, int w, int h, const char *t, int id)
{
    rrect(x, y, w, h, 8, gui_th->side);
    label(x + (w - ui_text_width(t, 12, UI_MEDIUM)) / 2, y + (h - 14) / 2, t, gui_th->text, 12, UI_MEDIUM);
    hit(x, y, w, h, id);
}

/* ---- Control Center ---- */

static void draw_control(void)
{
    const struct power_info *pw = power_probe();
    char b[48];
    int pad = 14, cw = (CCW - pad * 3) / 2, x1 = pad, x2 = pad * 2 + cw, y = pad;
    bool air = sys_prefs.airplane;

    ksnprintf(b, sizeof b, "%s", wifi_count() ? (air ? "Off (Airplane)" : sys_prefs.wifi_on ? "On" : "Off") : "No driver");
    tile(x1, y, cw, 62, "Wi-Fi", b, wifi_count() && !air && sys_prefs.wifi_on, H_WIFI, 0);
    ksnprintf(b, sizeof b, "%s", bt_count() ? (air ? "Off (Airplane)" : sys_prefs.bt_on ? "On" : "Off") : "No adapter");
    tile(x2, y, cw, 62, "Bluetooth", b, bt_count() && !air && sys_prefs.bt_on, H_BT, 1);
    y += 70;
    tile(x1, y, cw, 62, "Airplane Mode", air ? "On" : "Off", air, H_AIR, 2);
    tile(x2, y, cw, 62, "Ethernet", net_is_up() ? "Connected" : net_busy() ? "Connecting" : net_enabled() ? "No cable" : "Off", net_enabled(), H_ETH, 3);
    y += 70;
    tile(x1, y, cw, 62, "Do Not Disturb", sys_prefs.dnd ? "On" : "Off", sys_prefs.dnd, H_DND, 4);
    tile(x2, y, cw, 62, sys_prefs.dark ? "Dark Mode" : "Light Mode", "Tap to switch", sys_prefs.dark, H_DARK, 5);
    y += 78;
    ksnprintf(b, sizeof b, "%d%%", bri_disp);
    slider(pad, y, CCW - 2 * pad, bri_disp, 10, H_BRIGHT, "Display", b);
    y += 62;
    slider(pad, y, CCW - 2 * pad, sys_prefs.muted ? 0 : vol_disp, 0, H_VOL, "Sound",
           sys_prefs.muted ? "Muted - tap to unmute" : "PC speaker");
    hit(CCW - pad - 150, y, 150, 20, H_MUTE);
    y += 66;
    gfx_hline(ps, ox + pad, oy + y - 6, CCW - 2 * pad, gui_th->line);
    if (pw->battery)
        label(pad, y, "Battery detected (charge level unreadable)", gui_th->text, 12, UI_REGULAR);
    else
        label(pad, y, pw->mobile ? "Laptop: no battery device found" : "Power: mains, no battery found", gui_th->text, 12, UI_REGULAR);
    y += 26;
    button(x1, y, cw, 28, "Network Settings", H_SET_NET);
    button(x2, y, cw, 28, "Sound Settings", H_SET_SND);
    panel_h = y + 28 + pad;
}

/* ---- Notification Center ---- */

static const char *const month_names[12] = { "January", "February", "March", "April", "May", "June", "July",
                                              "August", "September", "October", "November", "December" };
static const char *const day_names[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const wd_short[7] = { "S", "M", "T", "W", "T", "F", "S" };

static void draw_notify(void)
{
    char b[96];
    int pad = 14, w = NCW - 2 * pad, y = pad;
    int wd = cal_weekday(today_y, today_m, today_d);

    ksnprintf(b, sizeof b, "%s", day_names[wd]);
    label(pad, y, b, gui_th->dim, 14, UI_MEDIUM);
    ksnprintf(b, sizeof b, "%d %s %d", today_d, month_names[(today_m + 11) % 12], today_y);
    label(pad, y + 18, b, gui_th->text, 24, UI_BOLD);
    y += 56;

    /* notifications */
    int n = notify_count();
    label(pad, y, "Notifications", gui_th->text, 14, UI_SEMIBOLD);
    if (n) {
        int tw = ui_text_width("Clear All", 12, UI_MEDIUM);
        label(NCW - pad - tw, y + 2, "Clear All", gui_th->accent, 12, UI_MEDIUM);
        hit(NCW - pad - tw - 6, y - 2, tw + 12, 22, H_CLEAR);
    }
    y += 26;
    if (!n) {
        rrect(pad, y, w, 40, 12, gui_th->side);
        label(pad + 14, y + 12, "No new notifications", gui_th->dim, 12, UI_REGULAR);
        y += 48;
    }
    for (int i = 0; i < n && i < 3; i++) {
        const struct notification *nt = notify_get(i);
        rrect(pad, y, w, 56, 12, gui_th->side);
        label(pad + 12, y + 7, nt->app, gui_th->dim, 12, UI_MEDIUM);
        ksnprintf(b, sizeof b, "%02d:%02d", nt->hour, nt->minute);
        label(pad + w - 12 - ui_text_width(b, 12, UI_REGULAR) - 18, y + 7, b, gui_th->dim, 12, UI_REGULAR);
        label(pad + 12, y + 23, nt->title, gui_th->text, 14, UI_SEMIBOLD);
        label(pad + 12, y + 40 - 2, nt->body, gui_th->dim, 12, UI_REGULAR);
        label(pad + w - 20, y + 5, "x", gui_th->dim, 14, UI_MEDIUM);
        hit(pad + w - 28, y, 28, 24, H_NOTI + i);
        y += 62;
    }
    if (n > 3) {
        ksnprintf(b, sizeof b, "+%d more", n - 3);
        label(pad + 4, y - 2, b, gui_th->dim, 12, UI_REGULAR);
        y += 18;
    }

    /* Do Not Disturb row */
    y += 4;
    rrect(pad, y, w, 38, 12, gui_th->side);
    label(pad + 14, y + 10, "Do Not Disturb", gui_th->text, 14, UI_MEDIUM);
    rrect(pad + w - 56, y + 8, 42, 22, 11, sys_prefs.dnd ? gui_th->accent : gui_th->line);
    disc(pad + w - 56 + (sys_prefs.dnd ? 31 : 11), y + 19, 8, GFX_RGB(255, 255, 255));
    hit(pad, y, w, 38, H_DNDROW);
    y += 50;

    /* calendar */
    ksnprintf(b, sizeof b, "%s %d", month_names[(vm + 11) % 12], vy);
    label(pad, y, b, gui_th->text, 16, UI_SEMIBOLD);
    label(NCW - pad - 50, y, "<", gui_th->accent, 16, UI_BOLD);
    hit(NCW - pad - 58, y - 4, 28, 26, H_PREV);
    label(NCW - pad - 16, y, ">", gui_th->accent, 16, UI_BOLD);
    hit(NCW - pad - 24, y - 4, 28, 26, H_NEXT);
    y += 28;
    int cell = w / 7;
    for (int i = 0; i < 7; i++)
        label(pad + i * cell + (cell - ui_text_width(wd_short[i], 12, UI_MEDIUM)) / 2, y, wd_short[i], gui_th->dim, 12, UI_MEDIUM);
    y += 20;
    int first = cal_weekday(vy, vm, 1), dim = cal_days_in_month(vy, vm);
    for (int d = 1; d <= dim; d++) {
        int pos = first + d - 1, cx = pad + (pos % 7) * cell + cell / 2, cy = y + (pos / 7) * 30 + 14;
        bool is_today = vy == today_y && vm == today_m && d == today_d;
        bool is_sel = d == sel_d;
        if (is_today) disc(cx, cy, 13, gui_th->accent);
        else if (is_sel) disc(cx, cy, 13, gui_th->line);
        ksnprintf(b, sizeof b, "%d", d);
        label(cx - ui_text_width(b, 14, UI_REGULAR) / 2, cy - 9, b, is_today ? gui_th->accent_fg : gui_th->text, 14, is_today ? UI_BOLD : UI_REGULAR);
        int idx[1];
        if (cal_on_day(vy, vm, d, idx, 1))
            disc(cx, cy + 11, 2, is_today ? gui_th->accent_fg : gui_th->accent);
        hit(cx - cell / 2, cy - 14, cell, 30, H_DAY + d);
    }
    y += ((first + dim + 6) / 7) * 30 + 8;

    /* events of the selected day */
    ksnprintf(b, sizeof b, "Events on %d %s", sel_d, month_names[(vm + 11) % 12]);
    label(pad, y, b, gui_th->text, 14, UI_SEMIBOLD);
    y += 24;
    int idx[4];
    int ne = cal_on_day(vy, vm, sel_d, idx, 4);
    if (!ne && !adding) {
        label(pad, y, "No events", gui_th->dim, 12, UI_REGULAR);
        y += 22;
    }
    for (int i = 0; i < ne; i++) {
        rrect(pad, y, w, 28, 8, gui_th->side);
        rrect(pad + 6, y + 6, 3, 16, 1, gui_th->accent);
        label(pad + 16, y + 7, cal_get(idx[i])->text, gui_th->text, 12, UI_REGULAR);
        label(pad + w - 18, y + 6, "x", gui_th->dim, 14, UI_MEDIUM);
        hit(pad + w - 28, y, 28, 28, H_EV + idx[i]);
        y += 32;
    }
    if (adding) {
        rrect(pad, y, w, 30, 8, gui_th->side);
        rrect(pad, y, w, 30, 8, gui_th->side);
        gfx_rect(ps, ox + pad, oy + y, w, 30, gui_th->accent);
        int tx = ui_text_width(addbuf, 12, UI_REGULAR);
        label(pad + 10, y + 8, addbuf, gui_th->text, 12, UI_REGULAR);
        if ((now_ms() / 500) & 1)
            gfx_fill_rect(ps, ox + pad + 12 + tx, oy + y + 7, 1, 16, gui_th->text);
        y += 36;
        label(pad, y, "Enter to save, Esc to cancel", gui_th->dim, 12, UI_REGULAR);
        y += 20;
    } else {
        button(pad, y + 2, w, 28, "Add Event", H_ADD);
        y += 34;
    }
    panel_h = y + pad;
}

/* ------------------------------------------------------------------ */

static void toast_draw(struct gfx_surface *s)
{
    if (notify_serial != seen_serial) {
        seen_serial = notify_serial;
        if (!sys_prefs.dnd && notify_count()) {
            toast = *notify_get(0);
            toast_on = true;
            toast_t0 = now_ms();
        }
    }
    if (!toast_on)
        return;
    int t = (int)(now_ms() - toast_t0), w = 320, h = 62;
    if (t > 4600) {
        toast_on = false;
        return;
    }
    int in = ease(MIN(t, 250), 250), out = t > 4350 ? ease(t - 4350, 250) : 0;
    int slide = (256 - in) * (w + 20) / 256 + out * (w + 20) / 256;
    int x = gfx_width() - w - 12 + slide, y = 36;
    gfx_fill_rect_alpha(s, x + 2, y + 5, w, h, GFX_BLACK, 50);
    gfx_fill_round_rect(s, x, y, w, h, 14, gui_th->win);
    gfx_fill_round_rect(s, x + 12, y + 14, 34, 34, 9, gui_th->accent);
    ui_text(s, x + 20, y + 19, "M", gui_th->accent_fg, 20, UI_BOLD);
    ui_text(s, x + 58, y + 9, toast.app, gui_th->dim, 12, UI_MEDIUM);
    ui_text(s, x + 58, y + 24, toast.title, gui_th->text, 14, UI_SEMIBOLD);
    ui_text(s, x + 58, y + 42, toast.body, gui_th->dim, 12, UI_REGULAR);
    gui_request_frames();
}

void panels_draw(struct gfx_surface *s)
{
    /* brightness eases toward the stored value (x10 gives smooth steps) */
    int want = sys_prefs.brightness * 10;
    if (bri_x10 < 0) bri_x10 = want;
    if (bri_x10 != want) {
        int d = want - bri_x10;
        bri_x10 += (d > 0 ? MAX(1, d / 6) : MIN(-1, d / 6));
        gfx_set_brightness((bri_x10 + 5) / 10);
        gui_request_frames();
    }
    if (drag != H_BRIGHT) bri_disp = sys_prefs.brightness;
    if (drag != H_VOL) vol_disp = sys_prefs.volume;
    toast_draw(s);

    if (which_open == PANEL_NONE)
        return;
    int t = (int)(now_ms() - t_change);
    int e = ease(MIN(t, 220), 220);
    if (closing) {
        e = 256 - e;
        if (t >= 220) { which_open = PANEL_NONE; closing = false; return; }
    }
    ps = s;
    nh = 0;
    panel_w = which_open == PANEL_CONTROL ? CCW : NCW;
    /* measure height first with a dry run into a hidden origin, then draw for real */
    ox = -4000; oy = 0;
    if (which_open == PANEL_CONTROL) draw_control(); else draw_notify();
    nh = 0;
    panel_h = MIN(panel_h, MAXH);
    panel_x = gfx_width() - panel_w - 8;
    int rest_y = 34;
    panel_y = rest_y - (256 - e) * 18 / 256;
    ox = panel_x; oy = panel_y;
    gfx_fill_rect_alpha(s, ox + 3, oy + 8, panel_w, panel_h, GFX_BLACK, 45 * e / 256);
    gfx_fill_round_rect(s, ox - 1, oy - 1, panel_w + 2, panel_h + 2, 19, gui_th->line);
    gfx_fill_round_rect(s, ox, oy, panel_w, panel_h, 18, gui_th->win);
    if (which_open == PANEL_CONTROL) draw_control(); else draw_notify();
    if (e < 256) {
        /* fade: dim the panel by laying the wallpaper-ish menu colour over it while it opens */
        gfx_fill_rect_alpha(s, ox, oy, panel_w, panel_h, gui_th->win, 256 - e);
    }
    if (t < 260) gui_request_frames();
    else if (which_open == PANEL_NOTIFY && adding) gui_request_frames();   /* caret blink */
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void set_brightness_from(int x)
{
    int h = 22, lx = panel_x + 14, w = CCW - 28;
    int v = 10 + (x - lx - h / 2) * 90 / (w - h);
    sys_prefs.brightness = MAX(10, MIN(100, v));
    bri_disp = sys_prefs.brightness;
}

static void set_volume_from(int x)
{
    int h = 22, lx = panel_x + 14, w = CCW - 28;
    int v = (x - lx - h / 2) * 100 / (w - h);
    sys_prefs.volume = MAX(0, MIN(100, v));
    sys_prefs.muted = sys_prefs.volume == 0;
    vol_disp = sys_prefs.volume;
}

static void click(int id)
{
    if (id >= H_EV) {
        cal_remove(id - H_EV);
    } else if (id >= H_NOTI) {
        notify_remove(id - H_NOTI);
    } else if (id >= H_DAY) {
        sel_d = id - H_DAY;
        adding = false;
    } else switch (id) {
    case H_WIFI:
        if (wifi_count() && !sys_prefs.airplane) { sys_prefs.wifi_on = !sys_prefs.wifi_on; panels_apply_radios(); }
        break;
    case H_BT:
        if (bt_count() && !sys_prefs.airplane) { sys_prefs.bt_on = !sys_prefs.bt_on; panels_apply_radios(); }
        break;
    case H_AIR:
        sys_prefs.airplane = !sys_prefs.airplane;
        panels_apply_radios();
        notify_post("Control Center", sys_prefs.airplane ? "Airplane Mode on" : "Airplane Mode off",
                    sys_prefs.airplane ? "Wi-Fi and Bluetooth are switched off" : "Wi-Fi and Bluetooth restored");
        break;
    case H_ETH:
        net_set_enabled(!net_enabled());
        break;
    case H_DND: case H_DNDROW:
        sys_prefs.dnd = !sys_prefs.dnd;
        break;
    case H_DARK:
        gui_set_dark(!sys_prefs.dark);
        break;
    case H_MUTE:
        sys_prefs.muted = !sys_prefs.muted;
        if (!sys_prefs.muted && sys_prefs.volume == 0) sys_prefs.volume = 50;
        break;
    case H_SET_NET: close_now(); settings_show("Network"); break;
    case H_SET_SND: close_now(); settings_show("Sound"); break;
    case H_CLEAR: notify_clear(); break;
    case H_PREV: if (--vm < 1) { vm = 12; vy--; } sel_d = 1; break;
    case H_NEXT: if (++vm > 12) { vm = 1; vy++; } sel_d = 1; break;
    case H_ADD: adding = true; addbuf[0] = 0; break;
    }
    sys_prefs_save();
    gui_request_frames();
}

bool panels_mouse(int x, int y, uint8_t buttons, uint8_t prev)
{
    bool down = (buttons & 1) && !(prev & 1);
    bool held = buttons & 1;

    if (toast_on && down) {
        int w = 320;
        if (x >= gfx_width() - w - 12 && y >= 36 && y < 98) {
            toast_on = false;
            if (which_open != PANEL_NOTIFY) panels_toggle(PANEL_NOTIFY);
            return true;
        }
    }
    if (drag) {
        if (!held) {
            drag = 0;
            sys_prefs_save();
            return true;
        }
        if (drag == H_BRIGHT) set_brightness_from(x); else set_volume_from(x);
        gui_request_frames();
        return true;
    }
    if (which_open == PANEL_NONE || closing)
        return false;
    bool inside = x >= panel_x && x < panel_x + panel_w && y >= panel_y && y < panel_y + panel_h;
    if (!inside) {
        if (down && y >= 28)
            close_now();                        /* click outside closes; the click is still used by the desktop */
        return false;
    }
    if (down) {
        int id = hit_at(x - panel_x, y - panel_y);
        if (id == H_BRIGHT || id == H_VOL) {
            drag = id;
            if (id == H_BRIGHT) set_brightness_from(x); else { sys_prefs.muted = false; set_volume_from(x); }
            gui_request_frames();
        } else if (id) {
            click(id);
        }
    }
    return true;
}

bool panels_key(int key)
{
    if (which_open == PANEL_NONE || closing)
        return false;
    if (adding) {
        int l = (int)strlen(addbuf);
        if (key == '\n' || key == '\r') {
            if (l) cal_add(vy, vm, sel_d, addbuf);
            adding = false;
        } else if (key == 27) {
            adding = false;
        } else if (key == '\b' || key == 127) {
            if (l) addbuf[l - 1] = 0;
        } else if (key >= 32 && key < 127 && l < 40) {
            addbuf[l] = (char)key; addbuf[l + 1] = 0;
        }
        gui_request_frames();
        return true;
    }
    if (key == 27) { close_now(); return true; }
    if (which_open == PANEL_NOTIFY) {
        int dim = cal_days_in_month(vy, vm);
        if (key == KEY_LEFT && sel_d > 1) sel_d--;
        else if (key == KEY_RIGHT && sel_d < dim) sel_d++;
        else if (key == KEY_UP && sel_d > 7) sel_d -= 7;
        else if (key == KEY_DOWN && sel_d + 7 <= dim) sel_d += 7;
        else if (key == KEY_PGUP) { if (--vm < 1) { vm = 12; vy--; } sel_d = 1; }
        else if (key == KEY_PGDN) { if (++vm > 12) { vm = 1; vy++; } sel_d = 1; }
        else if (key == 'n' || key == 'a') { adding = true; addbuf[0] = 0; }
        else return false;
        gui_request_frames();
        return true;
    }
    return false;
}
