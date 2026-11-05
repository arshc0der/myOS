/*
 * assistant.c - the MyOS Assistant: type a request, get an answer or an action.
 * (There is no microphone or speech engine, so it is a typed assistant. With "Voice assistant" on in
 * Settings, answers also play a short tone on the PC speaker.)
 *
 * Try: help, open files, time, date, dark mode, light mode, volume 40, mute, magnify on, reader on,
 *      computer name, who am i, network, check updates, restart, shut down.
 */
#include "guiapi.h"
#include "sys.h"
#include "input.h"
#include "net.h"
#include "timer.h"
#include "kernel.h"
#include "kstring.h"
#include "kprintf.h"

#define LINES 9
#define COLS 54

static char log_[LINES][COLS + 1];
static char inbuf[64];

static void say(const char *s)
{
    for (int i = 0; i + 1 < LINES; i++)
        strlcpy(log_[i], log_[i + 1], sizeof log_[i]);
    strlcpy(log_[LINES - 1], s, sizeof log_[0]);
    if (sys_prefs.voice)
        speaker_beep(660, 25);
}

static bool starts(const char *s, const char *p)
{
    while (*p) {
        char a = *s, b = *p;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return false;
        s++; p++;
    }
    return true;
}

static bool has(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    for (; *s; s++)
        if (starts(s, needle)) return true;
    (void)n;
    return false;
}

static void open_app(const char *name)
{
    int best = -1;
    for (int i = 0; i < gui_app_count(); i++) {
        const char *an = gui_app_at(i)->name;
        if (starts(an, name)) { best = i; break; }
        if (best < 0 && has(an, name)) best = i;
    }
    if (best < 0 || !name[0]) {
        say("I could not find that app.");
        return;
    }
    gui_open_named(gui_app_at(best)->name);
    char l[64];
    ksnprintf(l, sizeof l, "Opening %s.", gui_app_at(best)->name);
    say(l);
}

static void run(const char *q)
{
    char l[COLS + 1];
    uint32_t t[6];

    ksnprintf(l, sizeof l, "> %s", q);
    say(l);
    if (starts(q, "help")) {
        say("Try: open <app>, time, date, dark mode, volume N,");
        say("mute, magnify on/off, reader on/off, computer name,");
        say("who am i, network, check updates, restart, shut down.");
    } else if (starts(q, "open ") || starts(q, "launch ") || starts(q, "start ")) {
        const char *a = strchr(q, ' ') + 1;
        open_app(a);
    } else if (has(q, "time")) {
        sys_local_time(t);
        ksnprintf(l, sizeof l, "It is %02u:%02u in %s.", t[3], t[4], sys_prefs.city);
        say(l);
    } else if (has(q, "date") || has(q, "day")) {
        char d[16];
        sys_date_string(d, sizeof d);
        ksnprintf(l, sizeof l, "Today is %s.", d);
        say(l);
    } else if (has(q, "dark")) {
        gui_set_dark(true);
        say("Dark mode is on.");
    } else if (has(q, "light")) {
        gui_set_dark(false);
        say("Light mode is on.");
    } else if (starts(q, "volume")) {
        const char *n = q + 6;
        while (*n == ' ') n++;
        sys_prefs.volume = MAX(0, MIN(100, katoi(n)));
        sys_prefs.muted = false;
        sys_prefs_save();
        ksnprintf(l, sizeof l, "Volume is %d%%.", sys_prefs.volume);
        say(l);
    } else if (starts(q, "unmute")) {
        sys_prefs.muted = false;
        sys_prefs_save();
        say("Sound is on.");
    } else if (starts(q, "mute")) {
        sys_prefs.muted = true;
        sys_prefs_save();
        say("Sound is muted.");
    } else if (starts(q, "magnify")) {
        sys_prefs.acc_magnify = !has(q, "off");
        sys_prefs_save();
        say(sys_prefs.acc_magnify ? "Magnifier on." : "Magnifier off.");
    } else if (starts(q, "reader") || starts(q, "screen reader")) {
        sys_prefs.acc_reader = !has(q, "off");
        sys_prefs_save();
        say(sys_prefs.acc_reader ? "Screen reader on." : "Screen reader off.");
    } else if (has(q, "computer name") || has(q, "hostname")) {
        ksnprintf(l, sizeof l, "This computer is called %s.", sys_prefs.hostname);
        say(l);
    } else if (has(q, "who am i")) {
        ksnprintf(l, sizeof l, "You are %s.", users_current() ? users_current()->name : "nobody");
        say(l);
    } else if (has(q, "network") || has(q, "internet") || has(q, "wifi")) {
        say(net_status_text());
    } else if (has(q, "update")) {
        char m[128];
        sys_update_check(m, sizeof m);
        say(m);
    } else if (has(q, "restart") || has(q, "reboot")) {
        if (has(q, "now")) system_reboot();
        say("Say \"restart now\" to restart.");
    } else if (has(q, "shut")) {
        if (has(q, "now")) system_poweroff();
        say("Say \"shut down now\" to power off.");
    } else {
        say("Sorry, I did not understand. Type help.");
    }
}

static void as_open(struct gui_window *w)
{
    (void)w;
    memset(log_, 0, sizeof log_);
    inbuf[0] = 0;
    say("Hello! I am the MyOS Assistant. Type help.");
}

static void as_draw(struct gui_window *w)
{
    struct gfx_surface *s = w->surf;
    gfx_fill_rect(s, 0, 0, w->cw, w->ch, gui_th->win);
    for (int i = 0; i < LINES; i++)
        gfx_text(s, 14, 12 + i * 18, log_[i], log_[i][0] == '>' ? gui_th->dim : gui_th->text, GFX_TRANSPARENT, 1);
    gui_textfield(s, 14, w->ch - 44, w->cw - 28, inbuf, true);
}

static void as_key(struct gui_window *w, int key, int mods)
{
    int n = (int)strlen(inbuf);
    (void)w; (void)mods;
    if (key == '\n') {
        if (n) run(inbuf);
        inbuf[0] = 0;
    } else if ((key == '\b' || key == 127) && n) {
        inbuf[n - 1] = 0;
    } else if (key >= 32 && key < 127 && n < (int)sizeof inbuf - 1) {
        inbuf[n] = (char)key;
        inbuf[n + 1] = 0;
    }
    gui_redraw();
}

static void as_icon(struct gfx_surface *s, int x, int y, int sz, uint32_t g, uint32_t tile)
{
    gfx_fill_circle(s, x + sz / 2, y + sz / 2, sz * 30 / 100, g);
    gfx_fill_circle(s, x + sz / 2, y + sz / 2, sz * 18 / 100, tile);
    gfx_fill_circle(s, x + sz / 2, y + sz / 2, sz * 8 / 100, g);
}

static const struct gui_app assistant_app = {
    .name = "Assistant", .title = "Assistant", .cw = 440, .ch = 214, .single = true, .dock = 95,
    .open = as_open, .draw = as_draw, .key = as_key, .icon = as_icon,
};
GUI_APP(assistant_app);
