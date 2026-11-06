/*
 * setup.c - the first-boot Setup Assistant and the login screen.
 *
 * First boot (or after a factory reset) the desktop starts the assistant:
 *
 *   Welcome -> hardware check -> language -> region -> accessibility -> keyboard
 *   -> network -> privacy -> account -> computer name -> date & time -> preferences
 *   -> finishing -> welcome -> desktop
 *
 * Every screen has Back / Continue (and Skip when the step is optional), a progress bar
 * you can click to revisit a step, and works with mouse and keyboard.
 * Settings are written into sys_prefs as you go (and saved to /Users/.sysconf), so the
 * same values can be edited later in System Settings.
 *
 * Later boots show the login screen (gui_login) with the list of users, a password field and
 * Restart / Shut Down buttons.
 *
 * The code is one small immediate-mode UI: every frame redraws the screen and registers
 * clickable rectangles with hit(); the loop then maps a click or key to an action.
 */
#include "guiapi.h"
#include "uifont.h"
#include "display.h"
#include "sys.h"
#include "input.h"
#include "timer.h"
#include "hw.h"
#include "pmm.h"
#include "heap.h"
#include "blockdev.h"
#include "net.h"
#include "sched.h"
#include "kernel.h"
#include "kstring.h"
#include "kprintf.h"
#include "kerror.h"
#include "multiboot.h"

#define RED   GFX_RGB(220, 60, 50)
#define GREEN GFX_RGB(52, 199, 89)
#define AMBER GFX_RGB(255, 159, 10)

/* ------------------------------------------------------------------ */
/* Tiny UI toolkit: pointer, hit regions, fields                       */
/* ------------------------------------------------------------------ */

static int mx, my;
static uint8_t mbtn;
static int scw, sch;
static bool clicked;                    /* left button went down in this batch of events */

static struct { int x, y, w, h, id; } hits[96];
static int nhits;

static void hit_reset(void) { nhits = 0; }

static void hit(int x, int y, int w, int h, int id)
{
    if (nhits < 96) {
        hits[nhits].x = x; hits[nhits].y = y; hits[nhits].w = w; hits[nhits].h = h; hits[nhits].id = id;
        nhits++;
    }
}

static int hit_at(int x, int y)
{
    for (int i = nhits - 1; i >= 0; i--)
        if (gui_hit(x, y, hits[i].x, hits[i].y, hits[i].w, hits[i].h))
            return hits[i].id;
    return 0;
}

static void pointer_event(const struct input_event *ev)
{
    static const int mult[3] = { 2, 3, 5 };
    int ax = ev->dx, ay = ev->dy, sp = MAX(0, MIN(2, gui_prefs.ptr_speed));

    if (ax > 6 || ax < -6) ax *= 2;
    if (ay > 6 || ay < -6) ay *= 2;
    if (ev->pad & 1) {
        mx = MAX(0, MIN(scw - 1, (int)((int64_t)ev->dx * scw / 32768)));
        my = MAX(0, MIN(sch - 1, (int)((int64_t)ev->dy * sch / 32768)));
    } else {
        mx = MAX(0, MIN(scw - 1, mx + ax * mult[sp] / 2));
        my = MAX(0, MIN(sch - 1, my + ay * mult[sp] / 2));
    }
    if ((ev->buttons & MOUSE_LEFT) && !(mbtn & MOUSE_LEFT))
        clicked = true;
    mbtn = ev->buttons;
}

static const char *const arrow[19] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ", "X    X..X   ",
    "     X..X   ", "      X..X  ", "      X..X  ", "       XX   ",
};

static void draw_pointer(struct gfx_surface *s)
{
    for (int j = 0; j < 19; j++)
        for (int i = 0; i < 12; i++) {
            char c = arrow[j][i];
            if (c == 'X') gfx_pixel(s, mx + i, my + j, GFX_BLACK);
            else if (c == '.') gfx_pixel(s, mx + i, my + j, GFX_WHITE);
        }
}

static bool over(int id) { return hit_at(mx, my) == id; }

static void button(struct gfx_surface *s, int x, int y, int w, int h, const char *label, int id, bool primary)
{
    gui_button(s, x, y, w, h, label, primary);
    if (over(id) && !primary)
        gfx_fill_round_rect(s, x + 1, y + 1, w - 2, h - 2, 6, gfx_blend(gui_th->win, gui_th->accent, 24));
    if (over(id)) {
        gfx_text(s, x + (w - gfx_text_width(label, 1)) / 2, y + (h - 8) / 2, label,
                 primary ? gui_th->accent_fg : gui_th->text, GFX_TRANSPARENT, 1);
    }
    hit(x, y, w, h, id);
}

enum { H_BACK = 1, H_NEXT, H_SKIP, H_START, H_POWER, H_RESTART, H_SHUT, H_BTN1, H_BTN2, H_BTN3,
       H_DOT = 20, H_ROW = 100, H_TOG = 200, H_FLD = 300, H_SEG = 400, H_ACC = 420 };

static void row(struct gfx_surface *s, int x, int y, int w, const char *label, bool sel, int id)
{
    gui_sidebar_row(s, x, y, w, label, sel);
    if (!sel && over(id))
        gfx_fill_round_rect(s, x, y, w, 28, 6, gfx_blend(gui_th->win, gui_th->accent, 16));
    if (!sel && over(id))
        gfx_text(s, x + 10, y + 10, label, gui_th->text, GFX_TRANSPARENT, 1);
    hit(x, y, w, 30, id);
}

static void toggle(struct gfx_surface *s, int x, int y, int w, const char *label, const char *desc, bool on, bool focus, int id)
{
    if (focus)
        gfx_fill_round_rect(s, x - 6, y - 4, w + 12, 40, 8, gfx_blend(gui_th->win, gui_th->accent, 20));
    gfx_text(s, x, y + 2, label, gui_th->text, GFX_TRANSPARENT, 1);
    gfx_text(s, x, y + 18, desc, gui_th->dim, GFX_TRANSPARENT, 1);
    gui_switch(s, x + w - 40, y + 5, on);
    hit(x - 6, y - 4, w + 12, 40, id);
}

/* text fields */
struct fld { char *buf; int max; bool secret, name; const char *label; };
static struct fld fl[4];
static int nfl, focus;

static void mask(char *out, const char *in)
{
    int n = (int)strlen(in);
    if (n > 40) n = 40;
    for (int i = 0; i < n; i++)
        out[i] = '*';
    out[n] = 0;
}

static void draw_field(struct gfx_surface *s, int x, int y, int w, int i)
{
    char tmp[48];
    const char *t = fl[i].buf;

    gfx_text(s, x, y, fl[i].label, gui_th->dim, GFX_TRANSPARENT, 1);
    if (fl[i].secret) { mask(tmp, fl[i].buf); t = tmp; }
    gui_textfield(s, x, y + 14, w, t, focus == i);
    hit(x, y + 14, w, 28, H_FLD + i);
}

static bool edit_field(struct fld *f, int key)
{
    int n = (int)strlen(f->buf);

    if (key == '\b' || key == 127) {
        if (n) f->buf[n - 1] = 0;
        return true;
    }
    if (key >= 32 && key < 127 && n < f->max - 1) {
        if (f->name && !((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || key == '_' || key == '-' || key == '.'))
            return true;
        f->buf[n] = (char)key;
        f->buf[n + 1] = 0;
        return true;
    }
    return false;
}

static void background(struct gfx_surface *s)
{
    gfx_gradient_v(s, 0, 0, scw, sch, gui_th->bg_top, gui_th->bg_bot);
}

/* The lock screen shows your wallpaper, dimmed (like macOS). */
static void lock_background(struct gfx_surface *s)
{
    const struct gfx_surface *wp = gui_wallpaper_surface();

    if (wp && wp->w == s->w && wp->h == s->h) {
        for (int y = 0; y < s->h; y++)
            memcpy(s->px + y * s->stride, wp->px + y * wp->stride, (size_t)s->w * 4);
        gfx_fill_rect_alpha(s, 0, 0, s->w, s->h, GFX_BLACK, 110);
    } else {
        gfx_gradient_v(s, 0, 0, s->w, s->h, gfx_blend(gui_th->bg_top, GFX_BLACK, 120), gfx_blend(gui_th->bg_bot, GFX_BLACK, 120));
    }
}

static void card(struct gfx_surface *s, int cx, int cy, int cw, int ch)
{
    gfx_fill_round_rect(s, cx + 3, cy + 6, cw, ch, 16, gfx_blend(gui_th->bg_bot, GFX_BLACK, 40));
    gfx_fill_round_rect(s, cx - 1, cy - 1, cw + 2, ch + 2, 17, gui_th->line);
    gfx_fill_round_rect(s, cx, cy, cw, ch, 16, gui_th->win);
}

/* ------------------------------------------------------------------ */
/* Setup state                                                         */
/* ------------------------------------------------------------------ */

enum { S_WELCOME, S_POST, S_DISK, S_LANG, S_REGION, S_ACCESS, S_KEYB, S_NET, S_PRIV, S_ACCOUNT, S_NAME, S_TIME, S_PREFS, S_FINISH, S_COUNT };

static const char *const step_title[S_COUNT] = {
    "Welcome", "Hardware check", "Storage", "Language", "Country or region", "Accessibility", "Keyboard", "Network",
    "Data and privacy", "Create your account", "Computer name", "Date and time", "Final preferences", "Finishing",
};
static const char *const step_sub[S_COUNT] = {
    "", "MyOS is checking this computer.", "Keep your accounts and settings after a restart.", "Choose the language you want to use.",
    "This sets the time zone, date format and clock.", "Turn on features that make the computer easier to use.",
    "Pick your layout and check that your keys work.", "Connect to a network, or skip and do it later.",
    "Choose what MyOS may share. You can change this later.", "This account is the administrator of the computer.",
    "Your computer uses this name on the network.", "Choose your time zone and how dates and times look.",
    "A few choices to make the computer yours.", "",
};

static int step, maxstep, tick, cur;
static char errmsg[72];
static bool done_flag;

static char fullname[32], uname[24], pw1[24], pw2[24], host[24], wifipw[34];
static bool acct_admin = true;
static int net_sel_ap = -1;
static int disk_sel, disk_confirm;
static char disk_msg[72];
static struct wifi_ap aps[6];
static int nap;

/* keyboard test */
static char kt_text[30];
static int kt_count, kt_last = -1;
static bool post_key_seen;

/* hardware check */
struct post_line { const char *name; char val[72]; int state; };      /* 0 ok, 1 note, 2 problem */
static struct post_line post[9];
static int npost, post_shown;
static uint32_t post_t0;

/* network test */
static volatile int net_result = -3;                    /* -3 not run, -2 running, otherwise bit mask of passed checks (>=0) */

static const char *const layouts[5] = { "US English", "UK English", "German (QWERTZ)", "French (AZERTY)", "Hindi (Inscript)" };

static void apply_live(void)
{
    gui_prefs.dark = sys_prefs.dark;
    gui_prefs.accent = sys_prefs.acc_contrast ? GUI_ACCENTS - 1 : sys_prefs.accent;
    gui_prefs.anim = !sys_prefs.acc_reduce_motion;
    gui_theme_apply();
}

void gui_apply_sys_prefs(void)
{
    if (!sys_prefs.setup_done)
        return;
    apply_live();
}

static void post_add(const char *name, int state, const char *unused)
{
    (void)unused;
    if (npost < 9) {
        post[npost].name = name;
        post[npost].state = state;
        npost++;
    }
}

static void disk_cb(struct blockdev *d, void *ctx)
{
    uint32_t *acc = ctx;
    if (strcmp(d->kind, "partition") != 0 && strcmp(d->kind, "ram") != 0) {
        acc[0]++;
        acc[1] += d->sector_count / 2048;           /* MB */
    }
}

static void build_post(void)
{
    char brand[49];
    char hv[13];
    uint32_t disks[2] = { 0, 0 };
    struct pci_device pd[64];
    int npci = pci_scan(pd, 64), usbc = 0;
    uint32_t mb = pmm_total_pages() / 256;

    npost = 0;
    for (int i = 0; i < npci; i++)
        if (pd[i].cls == 0x0C && pd[i].sub == 0x03)
            usbc++;

    /* CPU */
    if (!cpu_brand(brand))
        strlcpy(brand, "x86 processor", sizeof brand);
    post_add("Processor", 0, "");
    {
        const char *t = brand;
        while (*t == ' ') t++;
        ksnprintf(post[0].val, sizeof post[0].val, "%s%s", t, cpu_hypervisor(hv) ? "  (virtual machine)" : "");
    }
    /* memory test: write and verify patterns in a block of RAM */
    {
        static const uint32_t pat[4] = { 0xAAAAAAAAu, 0x55555555u, 0x00000000u, 0xFFFFFFFFu };
        size_t n = 128 * 1024;
        uint32_t *m = kmalloc(n);
        bool ok = m != NULL;
        for (int p = 0; m && p < 4; p++) {
            for (size_t i = 0; i < n / 4; i++) m[i] = pat[p] ^ (uint32_t)i;
            for (size_t i = 0; i < n / 4; i++)
                if (m[i] != (pat[p] ^ (uint32_t)i)) { ok = false; break; }
        }
        if (m) kfree(m);
        post_add("Memory", ok ? 0 : 2, "");
        ksnprintf(post[npost - 1].val, sizeof post[0].val, "%u MB detected, quick test %s", mb, ok ? "passed" : "FAILED");
    }
    post_add("Firmware", 0, "");
    ksnprintf(post[npost - 1].val, sizeof post[0].val, "Started by the GRUB boot loader (Multiboot)");
    blockdev_for_each(disk_cb, disks);
    post_add("Storage", disks[0] ? 0 : 1, "");
    if (disks[0])
        ksnprintf(post[npost - 1].val, sizeof post[0].val, "%u disk%s, %u MB in total", disks[0], disks[0] == 1 ? "" : "s", disks[1]);
    else
        ksnprintf(post[npost - 1].val, sizeof post[0].val, "No disk found: your settings will not be kept after a restart");
    post_add("Keyboard", 1, "");
    post_add("Pointer", mouse_present() ? 0 : 1, "");
    ksnprintf(post[npost - 1].val, sizeof post[0].val, mouse_present() ? "Mouse or touchpad ready" : "No mouse found: use the keyboard (Tab, Enter, arrows)");
    post_add("Display", 0, "");
    {
        const struct display_info *di = display_get();
        if (di->refresh_hz)
            ksnprintf(post[npost - 1].val, sizeof post[0].val, "%d x %d, %d bpp, refresh %d Hz (measured)", gfx_width(), gfx_height(), di->bpp, di->refresh_hz);
        else
            ksnprintf(post[npost - 1].val, sizeof post[0].val, "%d x %d, %d bpp, refresh rate unknown", gfx_width(), gfx_height(), di->bpp);
    }
    post_add("Devices", 0, "");
    ksnprintf(post[npost - 1].val, sizeof post[0].val, "%d PCI devices, %d USB controller%s", npci, usbc, usbc == 1 ? "" : "s");
    {
        int nn = net_probe();
        post_add("Network", nn ? 0 : 1, "");
        ksnprintf(post[npost - 1].val, sizeof post[0].val, nn ? "%d Ethernet adapter%s found" : "No Ethernet adapter found", nn, nn == 1 ? "" : "s");
    }
    post_shown = 0;
    post_t0 = timer_ms();
}

static void kt_reset(void) { kt_text[0] = 0; kt_count = 0; kt_last = -1; }

static void set_fields(int st)
{
    nfl = 0;
    focus = 0;
    cur = 0;
    if (st == S_ACCOUNT) {
        fl[0] = (struct fld){ fullname, sizeof fullname, false, false, "Full name" };
        fl[1] = (struct fld){ uname, sizeof uname, false, true, "Account name (letters, digits, - _ .)" };
        fl[2] = (struct fld){ pw1, sizeof pw1, true, false, "Password" };
        fl[3] = (struct fld){ pw2, sizeof pw2, true, false, "Confirm password" };
        nfl = 4;
    } else if (st == S_NAME) {
        fl[0] = (struct fld){ host, sizeof host, false, true, "Computer name" };
        nfl = 1;
        if (!strcmp(host, "myos") && uname[0])
            ksnprintf(host, sizeof host, "%s-myos", uname);
    } else if (st == S_NET) {
        fl[0] = (struct fld){ wifipw, sizeof wifipw, true, false, "Wi-Fi password" };
        nfl = 1;
        focus = -1;
    } else if (st == S_POST) {
        build_post();
    } else if (st == S_KEYB) {
        kt_reset();
    }
}

static void go(int to)
{
    errmsg[0] = 0;
    step = to;
    if (to > maxstep)
        maxstep = to;
    set_fields(to);
    if (to >= S_LANG && to < S_FINISH)
        sys_prefs_save();                       /* keep what is done so far */
}

static bool skippable(int st)
{
    return st == S_DISK || st == S_ACCESS || st == S_KEYB || st == S_NET || st == S_PREFS || (st == S_ACCOUNT && users_count() > 0);
}

static void suggest_user(void)
{
    if (uname[0] || !fullname[0])
        return;
    int n = 0;
    for (const char *c = fullname; *c && *c != ' ' && n < 20; c++)
        if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9'))
            uname[n++] = *c;
        else if (*c >= 'A' && *c <= 'Z')
            uname[n++] = (char)(*c + 32);
    uname[n] = 0;
}

/* Check the input of the current step. Returns NULL when it is fine. */
static const char *validate(int st)
{
    if (st == S_ACCOUNT) {
        if (users_count() > 0 && !fullname[0] && !uname[0])
            return NULL;                        /* keep the existing account */
        if (!fullname[0]) { focus = 0; return "Please enter your full name."; }
        if (!uname[0]) { focus = 1; return "Please choose an account name."; }
        for (int i = 0; i < users_count(); i++)
            if (!strcmp(users_get(i)->name, uname)) { focus = 1; return "That account name is already used."; }
        if (sys_prefs.require_password && strlen(pw1) < 4) { focus = 2; return "Use a password with at least 4 characters."; }
        if (strcmp(pw1, pw2) != 0) { focus = 3; return "The two passwords do not match."; }
    } else if (st == S_NAME) {
        if (!sys_hostname_ok(host)) { focus = 0; return "Use letters, digits and - only (23 characters at most)."; }
    }
    return NULL;
}

static void next(void)
{
    const char *e;

    if (step == S_ACCOUNT && !uname[0])
        suggest_user();
    e = validate(step);
    if (e) {
        ksnprintf(errmsg, sizeof errmsg, "%s", e);
        return;
    }
    if (step == S_NAME)
        strlcpy(sys_prefs.hostname, host, sizeof sys_prefs.hostname);
    if (step + 1 < S_COUNT)
        go(step + 1);
}

static void prev(void)
{
    if (step > S_WELCOME && step < S_FINISH)
        go(step - 1);
}

static bool *tog_ptr(int st, int i)
{
    switch (st) {
    case S_ACCESS: { bool *p[5] = { &sys_prefs.acc_reader, &sys_prefs.acc_magnify, &sys_prefs.acc_contrast, &sys_prefs.acc_kbd_pointer, &sys_prefs.acc_reduce_motion }; return i < 5 ? p[i] : NULL; }
    case S_PRIV: { bool *p[3] = { &sys_prefs.priv_diag, &sys_prefs.priv_location, &sys_prefs.priv_analytics }; return i < 3 ? p[i] : NULL; }
    case S_PREFS: { bool *p[4] = { &sys_prefs.auto_updates, &sys_prefs.check_updates, &sys_prefs.voice, &sys_prefs.open_terminal }; return i < 4 ? p[i] : NULL; }
    case S_ACCOUNT: return i == 0 ? &acct_admin : i == 1 ? &sys_prefs.require_password : NULL;
    case S_TIME: return i == 0 ? &sys_prefs.auto_time : NULL;
    }
    return NULL;
}

static int tog_count(int st)
{
    return st == S_ACCESS ? 5 : st == S_PRIV ? 3 : st == S_PREFS ? 4 : st == S_TIME ? 1 : 0;
}

static void flip(int st, int i)
{
    bool *p = tog_ptr(st, i);
    if (p) {
        *p = !*p;
        apply_live();
    }
}

/* ---- network test (runs in its own task so the screen keeps moving) ---- */
static void net_test_task(void *arg)
{
    struct netdev *d = net_default();
    int r = 0;
    (void)arg;
    if (d && net_is_up()) {
        if (d->gw && net_ping(d->gw, 1500) >= 0) r |= 1;
        if (net_resolve("example.com")) r |= 2;
        if (net_ping(IP4(1, 1, 1, 1), 2000) >= 0) r |= 4;
    }
    net_result = r;
}

static void start_net_test(void)
{
    if (net_result == -2)
        return;
    if (!net_is_up()) {
        net_result = 0;
        return;
    }
    net_result = -2;
    task_create("nettest", net_test_task, NULL);
}

/* ------------------------------------------------------------------ */
/* Drawing the steps                                                   */
/* ------------------------------------------------------------------ */

static int CX, CY, CW, CH, BX, BY, BW;

static void draw_progress(struct gfx_surface *s)
{
    int n = S_PREFS - S_POST + 1;                       /* steps shown as dots */
    int seg = (CW - 72) / n;

    for (int i = 0; i < n; i++) {
        int st = S_POST + i;
        uint32_t c = st < step ? gui_th->accent : st == step ? gui_th->accent : gui_th->line;
        int x = CX + 36 + i * seg;
        gfx_fill_round_rect(s, x, CY + 16, seg - 6, 5, 2, st <= step ? c : gui_th->line);
        if (st <= maxstep && st != step && step != S_FINISH)
            hit(x, CY + 10, seg - 6, 16, H_DOT + st);
    }
}

static void draw_hint(struct gfx_surface *s, const char *t)
{
    gfx_text(s, BX, CY + CH - 76, t, gui_th->dim, GFX_TRANSPARENT, 1);
}

static void draw_welcome(struct gfx_surface *s)
{
    int n = sys_language_count();
    int phase = tick % 36, idx = (tick / 36) % n;
    int alpha = phase < 10 ? phase * 25 : phase > 26 ? (36 - phase) * 25 : 250;
    const char *g = (tick < 200 && sys_prefs.language == 0) ? sys_greetings[idx] : sys_greetings[sys_prefs.language];
    int tw;

    background(s);
    gfx_draw_logo(s, scw / 2 - 40, sch / 2 - 150, 80);
    tw = gfx_text_width(g, 6);
    gfx_text(s, (scw - tw) / 2, sch / 2 - 40, g, gfx_blend(gui_th->bg_bot, gui_th->text, MIN(255, alpha)), GFX_TRANSPARENT, 6);
    { char wl[48]; ksnprintf(wl, sizeof wl, "%s - MyOS", sys_tr("Welcome")); gui_label_center(s, 0, sch / 2 + 30, scw, wl, gui_th->dim); }
    button(s, scw / 2 - 90, sch / 2 + 70, 180, 38, sys_tr("Start Setup"), H_START, true);
    button(s, scw / 2 - 90, sch / 2 + 118, 180, 30, sys_tr("Quick Setup"), H_BTN2, false);
    gui_label_center(s, 0, sch / 2 + 160, scw, "Quick Setup: account \"user\" without a password and default settings", gui_th->dim);
}

static void draw_post(struct gfx_surface *s)
{
    int shown = post_shown, y = BY;

    if (!sys_prefs.acc_reduce_motion || shown < npost) {
        shown = (int)((timer_ms() - post_t0) / 280);
        if (shown > npost) shown = npost;
        if (post_shown < shown) post_shown = shown;
    }
    for (int i = 0; i < npost; i++, y += 30) {
        gfx_text(s, BX + 26, y + 2, post[i].name, gui_th->text, GFX_TRANSPARENT, 1);
        if (i < post_shown) {
            int st = post[i].state;
            const char *v = post[i].val;
            char kb[72];
            if (!strcmp(post[i].name, "Keyboard")) {
                ksnprintf(kb, sizeof kb, post_key_seen ? "Working: a key press was received" : "Press any key to test it (more tests on the next screens)");
                v = kb;
                st = post_key_seen ? 0 : 1;
            }
            gfx_fill_circle(s, BX + 8, y + 6, 5, st == 0 ? GREEN : st == 1 ? AMBER : RED);
            gfx_text(s, BX + 130, y + 2, v, gui_th->dim, GFX_TRANSPARENT, 1);
        } else {
            static const char *const spin = "|/-\\";
            char c[2] = { spin[(tick / 3) & 3], 0 };
            gfx_text(s, BX + 4, y + 2, c, gui_th->dim, GFX_TRANSPARENT, 1);
        }
    }
    gui_progress(s, BX, BY + 9 * 30 + 4, BW, 8, post_shown, npost);
    draw_hint(s, post_shown >= npost ? "Hardware check finished. Yellow means a note, not an error." : "Checking...");
}


static void draw_disk(struct gfx_surface *s)
{
    char names[8][16], l[72];
    uint32_t mb[8];
    int n = sys_disk_candidates(names, mb, 8);

    if (sys_users_persistent()) {
        gfx_fill_circle(s, BX + 8, BY + 8, 6, GREEN);
        gfx_text(s, BX + 24, BY + 4, "Storage is ready: accounts and settings are kept on disk.", gui_th->text, GFX_TRANSPARENT, 1);
        return;
    }
    gfx_text(s, BX, BY, "MyOS needs a disk to remember your account and settings.", gui_th->text, GFX_TRANSPARENT, 1);
    if (!n) {
        gfx_text(s, BX, BY + 28, "No disk found. Without one, setup runs again at every start.", AMBER, GFX_TRANSPARENT, 1);
        gfx_text(s, BX, BY + 52, "Add one and restart. QEMU:  make run  (creates myos-disk.img)", gui_th->dim, GFX_TRANSPARENT, 1);
        gfx_text(s, BX, BY + 68, "UTM or a real PC: attach an IDE/SATA-IDE disk or USB stick.", gui_th->dim, GFX_TRANSPARENT, 1);
        return;
    }
    gfx_text(s, BX, BY + 22, "Choose a disk. It will be ERASED and formatted as MyFS.", gui_th->dim, GFX_TRANSPARENT, 1);
    for (int i = 0; i < n; i++) {
        ksnprintf(l, sizeof l, "%s    %u MB", names[i], mb[i]);
        row(s, BX, BY + 44 + i * 32, 320, l, disk_sel == i, H_ROW + i);
    }
    if (disk_sel >= n) disk_sel = 0;
    button(s, BX, BY + 52 + n * 32, 260, 30, disk_confirm ? "Click again to ERASE and use it" : "Use this disk (erases it)", H_BTN1, disk_confirm != 0);
    if (disk_msg[0])
        gfx_text(s, BX, BY + 92 + n * 32, disk_msg, disk_msg[0] == 'F' ? RED : GREEN, GFX_TRANSPARENT, 1);
}

static void draw_lang(struct gfx_surface *s)
{
    int n = sys_language_count();
    for (int i = 0; i < n; i++)
        row(s, BX, BY + i * 32, 250, sys_languages[i], sys_prefs.language == i, H_ROW + i);
    int px = BX + 280, pw_ = BW - 280;
    gfx_fill_round_rect(s, px, BY, pw_, 200, 12, gui_th->side);
    const char *g = sys_greetings[sys_prefs.language];
    gfx_text(s, px + (pw_ - gfx_text_width(g, 4)) / 2, BY + 50, g, gui_th->text, GFX_TRANSPARENT, 4);
    gui_label_center(s, px, BY + 110, pw_, sys_languages[sys_prefs.language], gui_th->text);
    gui_label_center(s, px, BY + 140, pw_, "Preview of your language", gui_th->dim);
    gfx_text(s, BX, BY + 8 * 32 + 8, "The choice is saved. Menus are English in this version; translations can be added later.", gui_th->dim, GFX_TRANSPARENT, 1);
}

static void draw_region(struct gfx_surface *s)
{
    int n = sys_country_count();
    char l[80];
    const char *city; int tz;

    for (int i = 0; i < n; i++)
        row(s, BX + (i / 5) * (BW / 2), BY + (i % 5) * 32, BW / 2 - 12, sys_countries[i], sys_prefs.country == i, H_ROW + i);
    gfx_fill_round_rect(s, BX, BY + 176, BW, 76, 12, gui_th->side);
    (void)city; (void)tz;
    ksnprintf(l, sizeof l, "Time zone      %s (UTC%+d:%02d)", sys_prefs.city, sys_prefs.tz_minutes / 60, (sys_prefs.tz_minutes < 0 ? -sys_prefs.tz_minutes : sys_prefs.tz_minutes) % 60);
    gfx_text(s, BX + 16, BY + 190, l, gui_th->text, GFX_TRANSPARENT, 1);
    ksnprintf(l, sizeof l, "Date format    %s", sys_prefs.date_format == 0 ? "DD/MM/YYYY" : sys_prefs.date_format == 1 ? "MM/DD/YYYY" : "YYYY-MM-DD");
    gfx_text(s, BX + 16, BY + 210, l, gui_th->text, GFX_TRANSPARENT, 1);
    ksnprintf(l, sizeof l, "Clock          %s", sys_prefs.clock24 ? "24-hour" : "12-hour");
    gfx_text(s, BX + 16, BY + 230, l, gui_th->text, GFX_TRANSPARENT, 1);
}

static void draw_toggles(struct gfx_surface *s, int st, const char *const *lab, const char *const *desc, int n, int y0)
{
    for (int i = 0; i < n; i++)
        toggle(s, BX, y0 + i * 46, BW, lab[i], desc[i], *tog_ptr(st, i), cur == i, H_TOG + i);
}

static void draw_access(struct gfx_surface *s)
{
    static const char *const lab[5] = { "Screen reader", "Screen magnification", "High contrast", "Keyboard navigation", "Reduce motion" };
    static const char *const desc[5] = {
        "Captions and a tone for the window under the pointer", "A 3x lens next to the pointer",
        "Black and white interface with strong lines (works now)", "Move the pointer with Alt + arrow keys",
        "Turn off window and dock animations (works now)" };
    draw_toggles(s, S_ACCESS, lab, desc, 5, BY);
}

static void draw_keyb(struct gfx_surface *s)
{
    char l[64];

    gfx_text(s, BX, BY, "Detected: keyboard (PS/2 or USB HID)", gui_th->text, GFX_TRANSPARENT, 1);
    for (int i = 0; i < 5; i++)
        row(s, BX, BY + 22 + i * 32, 230, layouts[i], sys_prefs.kbd_layout == i, H_ROW + i);
    int px = BX + 260, pw_ = BW - 260;
    gfx_text(s, px, BY + 22, "Test your keys: type here", gui_th->dim, GFX_TRANSPARENT, 1);
    gui_textfield(s, px, BY + 38, pw_, kt_text, true);
    if (kt_last >= 0) {
        const char *nm = kt_last == '\n' ? "Enter" : kt_last == '\b' ? "Backspace" : kt_last == '\t' ? "Tab" : kt_last == ' ' ? "Space"
                       : kt_last == KEY_UP ? "Up" : kt_last == KEY_DOWN ? "Down" : kt_last == KEY_LEFT ? "Left" : kt_last == KEY_RIGHT ? "Right" : NULL;
        if (nm) ksnprintf(l, sizeof l, "Last key: %s", nm);
        else if (kt_last >= 33 && kt_last < 127) ksnprintf(l, sizeof l, "Last key: '%c'", kt_last);
        else ksnprintf(l, sizeof l, "Last key: code 0x%x", kt_last);
    } else
        ksnprintf(l, sizeof l, "Last key: none yet");
    gfx_text(s, px, BY + 80, l, gui_th->text, GFX_TRANSPARENT, 1);
    ksnprintf(l, sizeof l, "Keys received: %d  %s", kt_count, kt_count >= 5 ? "(keyboard works)" : "");
    gfx_text(s, px, BY + 100, l, kt_count >= 5 ? GREEN : gui_th->dim, GFX_TRANSPARENT, 1);
    gfx_text(s, px, BY + 130, "Up / Down change the layout.", gui_th->dim, GFX_TRANSPARENT, 1);
    gfx_text(s, px, BY + 146, "Layout is saved; key mapping is US for now.", gui_th->dim, GFX_TRANSPARENT, 1);
}

static void draw_net(struct gfx_surface *s)
{
    struct netdev *d;
    char l[96], ip[20];
    int y = BY;

    net_probe();
    d = net_default();
    gfx_text(s, BX, y, "Ethernet", gui_th->dim, GFX_TRANSPARENT, 1);
    y += 16;
    if (d) {
        net_format_ip(d->ip, ip, sizeof ip);
        ksnprintf(l, sizeof l, "%s  (%s)  link %s   %s", d->name, d->driver, d->link ? "up" : "down", net_is_up() ? ip : "not connected");
        gfx_text(s, BX, y, l, gui_th->text, GFX_TRANSPARENT, 1);
    } else
        gfx_text(s, BX, y, "No Ethernet adapter found (QEMU: -nic user,model=e1000).", gui_th->text, GFX_TRANSPARENT, 1);
    y += 20;
    gfx_text(s, BX, y, net_busy() ? "Status: asking the router for an address..." : net_status_text(), gui_th->dim, GFX_TRANSPARENT, 1);
    y += 24;
    button(s, BX, y, 150, 28, net_busy() ? "Connecting..." : "Connect (DHCP)", H_BTN1, false);
    button(s, BX + 160, y, 150, 28, net_result == -2 ? "Testing..." : "Test connection", H_BTN2, false);
    y += 38;
    if (net_result >= 0) {
        ksnprintf(l, sizeof l, "Gateway %s   DNS %s   Internet %s", net_result & 1 ? "ok" : "no", net_result & 2 ? "ok" : "no", net_result & 4 ? "ok" : "no");
        gfx_text(s, BX, y, l, net_result ? GREEN : AMBER, GFX_TRANSPARENT, 1);
        if (!net_result)
            gfx_text(s, BX, y + 16, net_is_up() ? "No reply (some emulators block ping)." : "Connect first.", gui_th->dim, GFX_TRANSPARENT, 1);
    }
    y += 46;
    gfx_text(s, BX, y, "Wi-Fi", gui_th->dim, GFX_TRANSPARENT, 1);
    y += 16;
    if (wifi_count()) {
        struct wifi_adapter *wa = wifi_get(0);
        button(s, BX, y, 90, 24, "Scan", H_BTN3, false);
        for (int i = 0; i < nap && i < 4; i++) {
            ksnprintf(l, sizeof l, "%s%s   %d%%", aps[i].ssid, aps[i].secured ? " (secured)" : "", aps[i].signal);
            row(s, BX + 100, y - 2 + i * 26, BW - 100, l, net_sel_ap == i, H_ROW + i);
        }
        if (net_sel_ap >= 0 && aps[net_sel_ap].secured) {
            draw_field(s, BX, y + 112, 260, 0);
        }
        (void)wa;
    } else {
        char hw[3][40];
        int nh = wifi_detect_hardware(hw, 3);
        gfx_text(s, BX, y, nh ? "A Wi-Fi radio was found but MyOS has no driver for it yet." : "No Wi-Fi hardware found (virtual machines use the wired adapter).", gui_th->text, GFX_TRANSPARENT, 1);
        for (int i = 0; i < nh; i++)
            gfx_text(s, BX + 8, y + 16 + i * 14, hw[i], gui_th->dim, GFX_TRANSPARENT, 1);
    }
}

static void draw_priv(struct gfx_surface *s)
{
    static const char *const lab[3] = { "Share diagnostic data", "Location services", "Share usage analytics" };
    static const char *const desc[3] = { "Crash information may be sent to the developer (no data is sent yet)",
        "Lets apps ask for your approximate location (time zone based)", "Anonymous statistics about which apps are used" };
    static const char *const policy[4] = {
        "Privacy policy: MyOS is a learning system. It does not send data anywhere: there is no TCP/IP",
        "client for services. These switches are stored in /Users/.sysconf and apps may read them.",
        "Your files stay on your disk. Passwords are stored as salted hashes (teaching grade only).",
        "By continuing you accept these choices. Change them any time in System Settings.",
    };
    draw_toggles(s, S_PRIV, lab, desc, 3, BY);
    gfx_fill_round_rect(s, BX, BY + 148, BW, 84, 10, gui_th->side);
    for (int i = 0; i < 4; i++)
        gfx_text(s, BX + 12, BY + 158 + i * 18, policy[i], i == 3 ? gui_th->text : gui_th->dim, GFX_TRANSPARENT, 1);
}

static void draw_account(struct gfx_surface *s)
{
    for (int i = 0; i < 4; i++)
        draw_field(s, BX, BY + i * 50, BW, i);
    toggle(s, BX, BY + 206, BW, "Administrator", "Can add users, change settings and erase the computer", acct_admin, focus == 4, H_TOG);
    toggle(s, BX, BY + 250, BW, "Require a password to log in", "Turn off to log in with a single click", sys_prefs.require_password, focus == 5, H_TOG + 1);
    if (users_count() > 0)
        gfx_text(s, BX, BY + 292, "An account already exists: Skip keeps it, or create another one here.", gui_th->dim, GFX_TRANSPARENT, 1);
    else if (!sys_users_persistent())
        gfx_text(s, BX, BY + 292, "No MyFS disk found: the account is lost on restart (setup runs again).", AMBER, GFX_TRANSPARENT, 1);
}

static void draw_name(struct gfx_surface *s)
{
    char l[64];

    draw_field(s, BX, BY + 10, BW, 0);
    ksnprintf(l, sizeof l, "Network name:  %s.local", host[0] ? host : "...");
    gfx_text(s, BX, BY + 70, l, gui_th->text, GFX_TRANSPARENT, 1);
    gfx_text(s, BX, BY + 96, "The name identifies this computer on the network and in the shell prompt.", gui_th->dim, GFX_TRANSPARENT, 1);
    gfx_text(s, BX, BY + 112, "Allowed: letters, digits and the minus sign.", gui_th->dim, GFX_TRANSPARENT, 1);
}

static int place_index(void)
{
    for (int i = 0; i < sys_place_count(); i++) {
        const char *c; int tz;
        sys_place_at(i, &c, &tz);
        if (!strcmp(c, sys_prefs.city))
            return i;
    }
    return 0;
}

static void draw_time(struct gfx_surface *s)
{
    static const char *const df[3] = { "DD/MM/YYYY", "MM/DD/YYYY", "YYYY-MM-DD" };
    static const char *const ck[2] = { "24-hour", "12-hour" };
    int sel = place_index(), n = sys_place_count();
    char l[64], d[16];
    uint32_t t[6];

    for (int i = 0; i < n; i++) {
        const char *c; int tz;
        sys_place_at(i, &c, &tz);
        row(s, BX + (i / 5) * (BW / 3), BY + (i % 5) * 32 - 4, BW / 3 - 10, c, sel == i, H_ROW + i);
    }
    gfx_text(s, BX, BY + 164, "Date format", gui_th->dim, GFX_TRANSPARENT, 1);
    gui_segment(s, BX, BY + 180, 300, df, 3, sys_prefs.date_format);
    for (int i = 0; i < 3; i++) hit(BX + i * 100, BY + 180, 100, 26, H_SEG + i);
    gfx_text(s, BX + 330, BY + 164, "Time format", gui_th->dim, GFX_TRANSPARENT, 1);
    gui_segment(s, BX + 330, BY + 180, 200, ck, 2, sys_prefs.clock24 ? 0 : 1);
    for (int i = 0; i < 2; i++) hit(BX + 330 + i * 100, BY + 180, 100, 26, H_SEG + 3 + i);
    toggle(s, BX, BY + 220, BW, "Set time automatically", "Use the computer's clock (a network time server is not used yet)", sys_prefs.auto_time, cur == 0, H_TOG);
    sys_local_time(t);
    sys_date_string(d, sizeof d);
    if (sys_prefs.clock24)
        ksnprintf(l, sizeof l, "Now: %s  %02u:%02u", d, t[3], t[4]);
    else
        ksnprintf(l, sizeof l, "Now: %s  %u:%02u %s", d, t[3] % 12 ? t[3] % 12 : 12, t[4], t[3] >= 12 ? "PM" : "AM");
    gfx_text(s, BX, BY + 270, l, gui_th->text, GFX_TRANSPARENT, 1);
}

static void draw_prefs(struct gfx_surface *s)
{
    static const char *const lab[4] = { "Install updates automatically", "Check for updates", "Voice assistant", "Open Terminal after login" };
    static const char *const desc[4] = { "Look on connected disks (MyOSUpdate) at startup", "Look for new versions when connected",
        "The Assistant app answers with a tone (typed requests)", "Show a Terminal window when the desktop starts" };
    static const char *const md[2] = { "Light", "Dark" };

    draw_toggles(s, S_PREFS, lab, desc, 4, BY);
    gfx_text(s, BX, BY + 196, "Appearance", gui_th->dim, GFX_TRANSPARENT, 1);
    gui_segment(s, BX, BY + 212, 200, md, 2, sys_prefs.dark ? 1 : 0);
    hit(BX, BY + 212, 100, 26, H_SEG);
    hit(BX + 100, BY + 212, 100, 26, H_SEG + 1);
    gfx_text(s, BX + 240, BY + 196, "Accent colour", gui_th->dim, GFX_TRANSPARENT, 1);
    for (int i = 0; i < GUI_ACCENTS; i++) {
        int cx = BX + 252 + i * 34, cy = BY + 226;
        if (i == sys_prefs.accent) gfx_fill_circle(s, cx, cy, 13, gui_th->text);
        gfx_fill_circle(s, cx, cy, i == sys_prefs.accent ? 11 : 13, gui_accent_color(i, sys_prefs.dark));
        hit(cx - 14, cy - 14, 28, 28, H_ACC + i);
    }
}

/* ---- finishing ---- */
static int fin_stage, fin_tick;
static bool fin_welcome;
static const char *const fin_text[6] = {
    "Checking your settings", "Creating your profile", "Preparing the file system", "Starting system services",
    "Loading the desktop", "Saving preferences",
};

static const char *finish_stage(int st)
{
    switch (st) {
    case 0:
        if (users_count() == 0 && (!fullname[0] || !uname[0]))
            return "No account was created.";
        if (!sys_hostname_ok(sys_prefs.hostname))
            return "The computer name is not valid.";
        break;
    case 1:
        if (uname[0]) {
            int r = users_add(uname, pw1, acct_admin);
            if (r < 0)
                return "The account could not be created.";
            users_set_fullname(uname, fullname);
        }
        break;
    case 2:
        if (uname[0])
            users_make_home(uname);
        break;
    case 3:
        if (net_probe())
            net_start_async();
        break;
    case 4:
        apply_live();
        break;
    case 5:
        sys_prefs.setup_done = true;
        sys_prefs_save();
        break;
    }
    return NULL;
}

static void draw_finish(struct gfx_surface *s)
{
    background(s);
    gfx_draw_logo(s, scw / 2 - 36, sch / 2 - 160, 72);
    if (!fin_welcome) {
        gui_label_center(s, 0, sch / 2 - 60, scw, sys_tr("Setting up your computer"), gui_th->text);
        for (int i = 0; i < 6; i++) {
            int y = sch / 2 - 30 + i * 22, x = scw / 2 - 130;
            uint32_t c = i < fin_stage ? gui_th->text : gui_th->dim;
            gfx_fill_circle(s, x, y + 4, 5, i < fin_stage ? GREEN : i == fin_stage ? AMBER : gui_th->line);
            gfx_text(s, x + 16, y, fin_text[i], c, GFX_TRANSPARENT, 1);
        }
        gui_progress(s, scw / 2 - 130, sch / 2 + 116, 260, 8, fin_stage * 10 + (fin_tick % 8) * 10 / 8, 60);
        if (errmsg[0])
            gui_label_center(s, 0, sch / 2 + 140, scw, errmsg, RED);
    } else {
        char l[64];
        const char *who = fullname[0] ? fullname : uname[0] ? uname : users_get(0)->name;
        ksnprintf(l, sizeof l, "%s, %s", sys_greetings[sys_prefs.language], who);
        gfx_text(s, (scw - gfx_text_width(l, 3)) / 2, sch / 2 - 60, l, gui_th->text, GFX_TRANSPARENT, 3);
        gui_label_center(s, 0, sch / 2 - 10, scw, sys_tr("Your computer is ready."), gui_th->dim);
        button(s, scw / 2 - 90, sch / 2 + 30, 180, 38, sys_tr("Get Started"), H_START, true);
        gui_label_center(s, 0, sch / 2 + 84, scw, "Settings you chose are in System Settings.", gui_th->dim);
    }
}

static void draw_step(struct gfx_surface *s)
{
    int gw = scw - 24, gh = sch - 24;

    CW = MIN(700, gw);
    CH = MIN(500, gh);
    CX = (scw - CW) / 2;
    CY = (sch - CH) / 2;
    BX = CX + 36;
    BW = CW - 72;
    BY = CY + 92;
    background(s);
    card(s, CX, CY, CW, CH);
    draw_progress(s);
    gfx_text(s, BX, CY + 36, sys_tr(step_title[step]), gui_th->text, GFX_TRANSPARENT, 2);
    gfx_text(s, BX, CY + 60, step_sub[step], gui_th->dim, GFX_TRANSPARENT, 1);
    switch (step) {
    case S_POST: draw_post(s); break;
    case S_DISK: draw_disk(s); break;
    case S_LANG: draw_lang(s); break;
    case S_REGION: draw_region(s); break;
    case S_ACCESS: draw_access(s); break;
    case S_KEYB: draw_keyb(s); break;
    case S_NET: draw_net(s); break;
    case S_PRIV: draw_priv(s); break;
    case S_ACCOUNT: draw_account(s); break;
    case S_NAME: draw_name(s); break;
    case S_TIME: draw_time(s); break;
    case S_PREFS: draw_prefs(s); break;
    }
    if (errmsg[0])
        gfx_text(s, BX, CY + CH - 62, errmsg, RED, GFX_TRANSPARENT, 1);
    /* footer */
    gfx_fill_rect(s, CX + 20, CY + CH - 48, CW - 40, 1, gui_th->line);
    button(s, CX + 36, CY + CH - 38, 100, 28, sys_tr("Back"), H_BACK, false);
    if (skippable(step))
        button(s, CX + CW - 36 - 100 - 110, CY + CH - 38, 100, 28, sys_tr("Skip"), H_SKIP, false);
    button(s, CX + CW - 36 - 100, CY + CH - 38, 100, 28, sys_tr(step == S_PREFS ? "Finish" : "Continue"), H_NEXT, true);
    char pr[24];
    ksnprintf(pr, sizeof pr, "Step %d of %d", step - S_POST + 1, S_PREFS - S_POST + 1);
    gui_label_center(s, CX + 150, CY + CH - 29, CW - 300 - 120, pr, gui_th->dim);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void click_id(int id)
{
    if (id >= H_DOT && id < H_DOT + S_COUNT) { go(id - H_DOT); return; }
    switch (id) {
    case H_BACK: prev(); return;
    case H_NEXT: next(); return;
    case H_SKIP:
        if (step == S_ACCOUNT) { fullname[0] = uname[0] = pw1[0] = pw2[0] = 0; }
        errmsg[0] = 0;
        if (step + 1 < S_COUNT) go(step + 1);
        return;
    case H_BTN2:
        if (step == S_WELCOME) {                       /* Quick Setup: defaults, account "user", no password */
            strlcpy(fullname, "User", sizeof fullname);
            strlcpy(uname, "user", sizeof uname);
            pw1[0] = pw2[0] = 0;
            sys_prefs.require_password = false;
            sys_auto_prepare();
            for (int i = 0; i < users_count(); i++)
                if (!strcmp(users_get(i)->name, "user")) { uname[0] = 0; break; }
            maxstep = S_FINISH;
            go(S_FINISH);
        }
        return;
    case H_START:
        if (step == S_WELCOME) go(S_POST);
        else if (step == S_FINISH && fin_welcome) done_flag = true;
        return;
    }
    if (id >= H_FLD && id < H_FLD + 4) { focus = id - H_FLD; return; }
    if (id >= H_TOG && id < H_TOG + 8) {
        int i = id - H_TOG;
        if (step == S_ACCOUNT) { focus = 4 + i; }
        else cur = i;
        flip(step, i);
        return;
    }
    if (id >= H_ACC && id < H_ACC + GUI_ACCENTS) { sys_prefs.accent = id - H_ACC; apply_live(); return; }
    if (id >= H_SEG && id < H_SEG + 6) {
        int i = id - H_SEG;
        if (step == S_PREFS) { sys_prefs.dark = i == 1; apply_live(); }
        else if (step == S_TIME) {
            if (i < 3) sys_prefs.date_format = i;
            else sys_prefs.clock24 = i == 3;
        }
        return;
    }
    if (id >= H_ROW && id < H_ROW + 40) {
        int i = id - H_ROW;
        switch (step) {
        case S_DISK: disk_sel = i; disk_confirm = 0; break;
        case S_LANG: sys_prefs.language = i; break;
        case S_REGION: sys_country_apply(i); break;
        case S_KEYB: sys_prefs.kbd_layout = i; break;
        case S_TIME: sys_place_set(i); break;
        case S_NET: net_sel_ap = i; focus = (i < nap && aps[i].secured) ? 0 : -1; break;
        }
        return;
    }
    if (step == S_DISK && id == H_BTN1) {
        char names[8][16]; uint32_t mb[8];
        int n = sys_disk_candidates(names, mb, 8);
        if (disk_sel < n) {
            if (!disk_confirm) { disk_confirm = 1; return; }
            disk_confirm = 0;
            if (sys_use_disk_for_users(names[disk_sel]) == 0)
                ksnprintf(disk_msg, sizeof disk_msg, "Done. Settings are saved on %s.", names[disk_sel]);
            else
                ksnprintf(disk_msg, sizeof disk_msg, "Failed: the disk could not be formatted.");
        }
        return;
    }
    if (step == S_NET) {
        if (id == H_BTN1) net_start_async();
        else if (id == H_BTN2) start_net_test();
        else if (id == H_BTN3 && wifi_count()) {
            struct wifi_adapter *wa = wifi_get(0);
            nap = wa->scan ? wa->scan(wa, aps, 6) : 0;
            if (nap < 0) nap = 0;
            net_sel_ap = -1;
        }
    }
}

static void list_move(int d)
{
    switch (step) {
    case S_LANG: sys_prefs.language = MAX(0, MIN(sys_language_count() - 1, sys_prefs.language + d)); break;
    case S_REGION: sys_country_apply(MAX(0, MIN(sys_country_count() - 1, sys_prefs.country + d))); break;
    case S_KEYB: sys_prefs.kbd_layout = MAX(0, MIN(4, sys_prefs.kbd_layout + d)); break;
    case S_TIME: sys_place_set(MAX(0, MIN(sys_place_count() - 1, place_index() + d))); break;
    }
}

static void key_step(int k)
{
    if (step == S_WELCOME) {
        if (k == '\n' || k == ' ') go(S_POST);
        return;
    }
    if (step == S_FINISH) {
        if (fin_welcome && (k == '\n' || k == ' '))
            done_flag = true;
        return;
    }
    if (step == S_POST)
        post_key_seen = true;
    if (step == S_KEYB && k != KEY_UP && k != KEY_DOWN && k != 27) {
        kt_count++;
        kt_last = k;
        if (k == '\b') { int n = (int)strlen(kt_text); if (n) kt_text[n - 1] = 0; }
        else if (k >= 32 && k < 127 && strlen(kt_text) < sizeof kt_text - 1) {
            int n = (int)strlen(kt_text);
            kt_text[n] = (char)k; kt_text[n + 1] = 0;
        }
        if (k == '\n' && kt_count >= 5)
            next();
        return;
    }
    if (k == 27) { prev(); return; }
    if (step == S_NET && focus == 0 && net_sel_ap >= 0 && aps[net_sel_ap].secured) {
        if (k == '\n') {
            struct wifi_adapter *wa = wifi_get(0);
            if (wa && wa->connect) wa->connect(wa, aps[net_sel_ap].ssid, wifipw);
            return;
        }
        if (edit_field(&fl[0], k)) return;
    }
    /* fields (account, name) */
    if (nfl > 0 && step != S_NET) {
        int total = step == S_ACCOUNT ? 6 : nfl;
        if (k == '\t' || k == KEY_DOWN) { if (step == S_ACCOUNT && focus == 0) suggest_user(); focus = (focus + 1) % total; return; }
        if (k == KEY_UP) { focus = (focus + total - 1) % total; return; }
        if (focus < nfl) {
            if (k == '\n') {
                if (focus < nfl - 1) { if (step == S_ACCOUNT && focus == 0) suggest_user(); focus++; }
                else next();
                return;
            }
            edit_field(&fl[focus], k);
            return;
        }
        if (k == ' ') { flip(step, focus - 4); return; }
        if (k == '\n') { next(); return; }
        return;
    }
    if (k == '\n') { next(); return; }
    if (k == KEY_UP) { if (tog_count(step) && step != S_TIME) cur = MAX(0, cur - 1); else list_move(-1); return; }
    if (k == KEY_DOWN || k == '\t') { if (tog_count(step) && step != S_TIME) cur = MIN(tog_count(step) - 1, cur + 1); else list_move(1); return; }
    if ((k == ' ' || k == KEY_LEFT || k == KEY_RIGHT) && tog_count(step) && step != S_TIME) { flip(step, cur); return; }
    if (step == S_TIME) {
        if (k == KEY_LEFT) sys_prefs.date_format = (sys_prefs.date_format + 2) % 3;
        else if (k == KEY_RIGHT) sys_prefs.date_format = (sys_prefs.date_format + 1) % 3;
        else if (k == 't' || k == 'T') sys_prefs.clock24 = !sys_prefs.clock24;
        else if (k == 'a' || k == 'A') sys_prefs.auto_time = !sys_prefs.auto_time;
    }
    if (step == S_PREFS && (k == 'd' || k == 'D')) { sys_prefs.dark = !sys_prefs.dark; apply_live(); }
    if (step == S_NET) {
        if (k == 'c' || k == 'C') net_start_async();
        else if (k == 't' || k == 'T') start_net_test();
    }
}

static void init_state(void)
{
    struct sys_prefs d = sys_prefs;
    (void)d;
    step = S_WELCOME;
    maxstep = S_WELCOME;
    tick = 0;
    errmsg[0] = 0;
    fullname[0] = uname[0] = pw1[0] = pw2[0] = wifipw[0] = 0;
    strlcpy(host, sys_prefs.hostname[0] ? sys_prefs.hostname : "myos", sizeof host);
    acct_admin = true;
    net_sel_ap = -1;
    nap = 0;
    disk_sel = disk_confirm = 0;
    disk_msg[0] = 0;
    net_result = -3;
    fin_stage = fin_tick = 0;
    fin_welcome = false;
    done_flag = false;
    post_key_seen = false;
    clicked = false;
    mbtn = 0;
    scw = gfx_width();
    sch = gfx_height();
    mx = scw / 2;
    my = sch / 2;
}

static void login_screen(void);
static const char *lock_start_user;           /* lock screen: whose avatar starts selected */

void gui_setup_assistant(void)
{
    struct gfx_surface *s = gfx_screen();

    init_state();
    sys_prefs.setup_done = false;
    apply_live();
    input_flush();

    while (!done_flag) {
        struct input_event ev;
        int r;

        hit_reset();
        if (step == S_WELCOME) draw_welcome(s);
        else if (step == S_FINISH) draw_finish(s);
        else draw_step(s);
        draw_pointer(s);
        gfx_flush_all();

        r = input_wait(&ev, 50);
        tick++;
        clicked = false;
        while (r == 0) {
            if (ev.type == INPUT_MOUSE)
                pointer_event(&ev);
            else if (ev.type == INPUT_KEY)
                key_step(ev.key);
            r = input_poll(&ev) ? 0 : -1;
        }
        if (clicked) {
            int id = hit_at(mx, my);
            if (id)
                click_id(id);
            else if (step >= S_LANG && step < S_FINISH)
                focus = (step == S_NET) ? -1 : focus;
        }
        if (step == S_FINISH) {
            if (!fin_welcome) {
                if (++fin_tick % 8 == 0) {
                    const char *e = finish_stage(fin_stage);
                    if (e) {
                        ksnprintf(errmsg, sizeof errmsg, "%s Going back.", e);
                        draw_finish(s);
                        gfx_flush_all();
                        timer_sleep_ms(1500);
                        go(S_ACCOUNT);
                        continue;
                    }
                    if (++fin_stage >= 6) {
                        fin_welcome = true;
                        speaker_beep(880, 120);
                    }
                }
            } else if (fin_tick++ > 400)
                done_flag = true;
        }
    }
    input_flush();
    sys_prefs.setup_done = true;
    if (uname[0])
        users_login(uname);
    else
        login_screen();
}

/* ------------------------------------------------------------------ */
/* Login screen                                                        */
/* ------------------------------------------------------------------ */

static void login_screen(void)
{
    struct gfx_surface *s = gfx_screen();
    int sel = 0, nusers = users_count();
    char pw[24] = "", err[48] = "";
    bool power = false;

    scw = gfx_width();
    sch = gfx_height();
    mx = scw / 2; my = sch / 2; mbtn = 0;
    apply_live();
    input_flush();
    if (lock_start_user)
        for (int i = 0; i < nusers; i++)
            if (!strcmp(users_get(i)->name, lock_start_user))
                sel = i;
    for (int t = 0;; t++) {
        int n = nusers > 5 ? 5 : nusers;
        char hid[28], row_t[64];
        uint32_t tm[6];
        struct input_event ev;
        int r;

        hit_reset();
        lock_background(s);
        sys_local_time(tm);
        {   /* big clock and date, like the macOS lock screen */
            char clk[16], dt[32];
            if (sys_prefs.clock24)
                ksnprintf(clk, sizeof clk, "%02u:%02u", tm[3], tm[4]);
            else
                ksnprintf(clk, sizeof clk, "%u:%02u", tm[3] % 12 ? tm[3] % 12 : 12, tm[4]);
            sys_date_string(dt, sizeof dt);
            int cw2 = ui_text_width(clk, 96, UI_THIN);
            ui_text(s, (scw - cw2) / 2, sch / 12, clk, GFX_WHITE, 96, UI_THIN);
            int dw2 = ui_text_width(dt, 20, UI_MEDIUM);
            ui_text(s, (scw - dw2) / 2, sch / 12 + ui_text_height(96, UI_THIN) + 4, dt, GFX_RGB(235, 238, 245), 20, UI_MEDIUM);
        }
        int ay = sch * 52 / 100;
        for (int i = 0; i < n; i++) {                    /* avatars: the chosen one is large with a ring */
            const struct sys_user *u = users_get(i);
            int rad = i == sel ? 44 : 26;
            int axc = scw / 2 + (i - (n - 1) / 2) * 104 - (n % 2 ? 0 : 52);
            int ayc = ay - (i == sel ? 0 : -18);
            if (i == sel)
                gfx_fill_circle(s, axc, ayc, rad + 3, GFX_WHITE);
            gfx_fill_circle(s, axc, ayc, rad, gfx_blend(gui_th->accent, GFX_BLACK, 40 + 20 * (i % 3)));
            char ini[2] = { (char)((u->full[0] ? u->full[0] : u->name[0]) & ~0x20), 0 };
            int iw = ui_text_width(ini, i == sel ? 40 : 24, UI_SEMIBOLD);
            ui_text(s, axc - iw / 2, ayc - (i == sel ? 24 : 14), ini, GFX_WHITE, i == sel ? 40 : 24, UI_SEMIBOLD);
            hit(axc - rad, ayc - rad, 2 * rad, 2 * rad, H_ROW + i);
        }
        {
            const struct sys_user *u = users_get(sel);
            ksnprintf(row_t, sizeof row_t, "%s", u->full[0] ? u->full : u->name);
            int nw = ui_text_width(row_t, 24, UI_SEMIBOLD);
            ui_text(s, (scw - nw) / 2, ay + 56, row_t, GFX_WHITE, 24, UI_SEMIBOLD);
        }
        int py = ay + 96, fw = 240, fx = (scw - fw) / 2;
        if (sys_prefs.require_password) {
            mask(hid, pw);
            gfx_fill_round_rect(s, fx, py, fw, 32, 16, GFX_RGB(255, 255, 255));
            gfx_fill_round_rect(s, fx + 1, py + 1, fw - 2, 30, 15, GFX_RGB(70, 74, 86));
            if (hid[0])
                ui_text(s, fx + 16, py + 8, hid, GFX_WHITE, 14, UI_REGULAR);
            else
                ui_text(s, fx + 16, py + 8, sys_tr("Enter password"), GFX_RGB(200, 204, 214), 14, UI_REGULAR);
            if ((t / 3) & 1)
                gfx_fill_rect(s, fx + 16 + ui_text_width(hid, 14, UI_REGULAR), py + 8, 1, 16, GFX_WHITE);
            gfx_fill_circle(s, fx + fw - 17, py + 16, 11, GFX_WHITE);          /* arrow button */
            gfx_text(s, fx + fw - 21, py + 12, ">", GFX_BLACK, GFX_TRANSPARENT, 1);
            hit(fx, py, fw, 32, H_NEXT);
        } else {
            gfx_fill_round_rect(s, fx, py, fw, 32, 16, GFX_RGB(255, 255, 255));
            ui_text(s, fx + (fw - ui_text_width(sys_tr("Log In"), 14, UI_SEMIBOLD)) / 2, py + 8, sys_tr("Log In"), GFX_BLACK, 14, UI_SEMIBOLD);
            hit(fx, py, fw, 32, H_NEXT);
        }
        {
            const char *msg = err[0] ? err : sys_prefs.require_password ? sys_tr("Enter your password") : "";
            int mw = ui_text_width(msg, 12, UI_REGULAR);
            ui_text(s, (scw - mw) / 2, py + 44, msg, err[0] ? GFX_RGB(255, 140, 140) : GFX_RGB(215, 219, 228), 12, UI_REGULAR);
        }
        {   /* bottom: restart and shut down as plain labels with a round button each */
            static const char *lab[2] = { "Restart", "Shut Down" };
            static const int ids[2] = { H_RESTART, H_SHUT };
            for (int i = 0; i < 2; i++) {
                const char *l = sys_tr(lab[i]);
                int lw = ui_text_width(l, 12, UI_MEDIUM), bx = scw / 2 + (i ? 30 : -30 - 44);
                gfx_fill_circle(s, bx + 22, sch - 78, 20, GFX_RGB(70, 74, 86));
                gfx_fill_circle(s, bx + 22, sch - 78, 7, GFX_WHITE);
                gfx_fill_circle(s, bx + 22, sch - 78, 5, GFX_RGB(70, 74, 86));
                gfx_fill_rect(s, bx + 21, sch - 90, 2, 10, GFX_WHITE);
                ui_text(s, bx + 22 - lw / 2, sch - 50, l, GFX_RGB(235, 238, 245), 12, UI_MEDIUM);
                hit(bx, sch - 100, 44, 70, ids[i]);
            }
        }
        draw_pointer(s);
        gfx_flush_all();

        r = input_wait(&ev, 200);
        clicked = false;
        int action = 0;
        while (r == 0) {
            if (ev.type == INPUT_MOUSE) {
                pointer_event(&ev);
            } else if (ev.type == INPUT_KEY) {
                int k = ev.key;
                if (k == KEY_UP && sel > 0) sel--;
                else if (k == KEY_DOWN && sel + 1 < nusers) sel++;
                else if (k == '\t') sel = (sel + 1) % nusers;
                else if (k == '\n') action = H_NEXT;
                else if (k == KEY_F(12) && (ev.mods & MOD_ALT)) action = H_SHUT;
                else {
                    struct fld f = { pw, sizeof pw, true, false, "" };
                    edit_field(&f, k);
                }
                err[0] = 0;
            }
            r = input_poll(&ev) ? 0 : -1;
        }
        if (clicked) {
            int id = hit_at(mx, my);
            if (id >= H_ROW && id < H_ROW + 8) { sel = id - H_ROW; err[0] = 0; }
            else if (id == H_NEXT || id == H_RESTART || id == H_SHUT) action = id;
        }
        if (action == H_NEXT) {
            if (!sys_prefs.require_password || users_check(users_get(sel)->name, pw)) {
                users_login(users_get(sel)->name);
                return;
            }
            ksnprintf(err, sizeof err, "%s", sys_tr("Wrong password. Try again."));
            pw[0] = 0;
        } else if (action == H_RESTART) {
            system_reboot();
        } else if (action == H_SHUT) {
            system_poweroff();
        }
        (void)power;
    }
}

void gui_lock_screen(void)
{
    lock_start_user = users_current() ? users_current()->name : NULL;
    login_screen();
    lock_start_user = NULL;
}

void gui_login(void)
{
    if (boot_has_arg("guitest") || boot_has_arg("nosetup")) {   /* automatic: no screens, demo account */
        if (users_count() == 0)
            users_add("demo", "", true);
        sys_prefs.setup_done = true;
        users_login(users_get(0)->name);
        return;
    }
    if (!sys_prefs.setup_done || users_count() == 0)
        gui_setup_assistant();
    else
        login_screen();
}
