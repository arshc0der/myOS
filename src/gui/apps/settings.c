/* settings.c - "Settings": appearance, pointer, display and connected devices. */
#include "guiapi.h"
#include "input.h"
#include "usb.h"
#include "kprintf.h"
#include "kstring.h"

#define SIDE_W 140

struct settings { int page; };

static const char *pages[] = { "Appearance", "Pointer", "Display", "Devices" };
#define NPAGES 4
#define X0 (SIDE_W + 24)
#define SEGW 300

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

static void settings_draw(struct gui_window *w)
{
    struct settings *st = w->priv;
    struct gfx_surface *s = w->surf;
    char line[96];

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, gui_th->win);
    gfx_fill_rect(s, 0, 0, SIDE_W, w->ch, gui_th->side);
    gfx_vline(s, SIDE_W, 0, w->ch, gui_th->line);
    for (int i = 0; i < NPAGES; i++)
        gui_sidebar_row(s, 0, 14 + i * 28, SIDE_W, pages[i], i == st->page);

    gfx_text(s, X0, 20, pages[st->page], gui_th->text, GFX_TRANSPARENT, 2);
    switch (st->page) {
    case 0: {
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
        break;
    }
    case 1: {
        const char *a[] = { "Slow", "Normal", "Fast" };
        heading(s, 64, "Tracking speed");
        gui_segment(s, X0, 80, SEGW, a, 3, gui_prefs.ptr_speed);
        gfx_text(s, X0, 126, "Tablets and touch screens follow the", gui_th->dim, GFX_TRANSPARENT, 1);
        gfx_text(s, X0, 140, "host pointer exactly; speed applies to", gui_th->dim, GFX_TRANSPARENT, 1);
        gfx_text(s, X0, 154, "mice and touchpads.", gui_th->dim, GFX_TRANSPARENT, 1);
        break;
    }
    case 2:
        ksnprintf(line, sizeof line, "%d x %d pixels", gui_screen_w(), gui_screen_h());
        heading(s, 64, "Resolution");
        gfx_text(s, X0, 80, line, gui_th->text, GFX_TRANSPARENT, 2);
        gfx_text(s, X0, 130, "The boot loader picks the screen size.", gui_th->dim, GFX_TRANSPARENT, 1);
        gfx_text(s, X0, 144, "To change it, restart and open", gui_th->dim, GFX_TRANSPARENT, 1);
        gfx_text(s, X0, 158, "\"Display resolution\" in the boot menu.", gui_th->dim, GFX_TRANSPARENT, 1);
        {
            const char *f[] = { "Auto", "RGB", "BGR" };
            heading(s, 196, "Colour order (use if colours look wrong)");
            gui_segment(s, X0, 212, SEGW, f, 3, gui_prefs.pixfmt);
        }
        break;
    case 3: {
        heading(s, 64, "USB devices");
        int y = 84, n = 0;
        for (int i = 0; usb_describe(i, line, sizeof line); i++) {
            gfx_text(s, X0, y, line, gui_th->text, GFX_TRANSPARENT, 1);
            y += 18;
            n++;
        }
        if (!n)
            gfx_text(s, X0, y, "No USB devices found", gui_th->dim, GFX_TRANSPARENT, 1);
        y += 28;
        heading(s, y, "PS/2");
        gfx_text(s, X0, y + 16, mouse_present() ? "PS/2 mouse present" : "No PS/2 mouse", gui_th->text,
                 GFX_TRANSPARENT, 1);
        break;
    }
    }
}

static void settings_click(struct gui_window *w, int x, int y, int buttons)
{
    struct settings *st = w->priv;
    (void)buttons;

    if (x < SIDE_W) {
        int i = (y - 14) / 28;
        if (y >= 14 && i >= 0 && i < NPAGES)
            st->page = i;
    } else if (x >= X0 && x < X0 + SEGW) {
        if (st->page == 0) {
            int i = (x - X0) / (SEGW / 2);
            if (y >= 80 && y < 106) {
                gui_set_dark(i == 1);
            } else if (y >= 134 && y < 160) {                  /* style: colourful or monochrome */
                gui_prefs.accent = i == 1 ? GUI_ACCENTS - 1 : 0;
                gui_prefs.color_buttons = i == 0;
                gui_refresh_theme();
            } else if (y >= 187 && y < 220) {
                int k = (x - X0) / 42;
                if (k >= 0 && k < GUI_ACCENTS) {
                    gui_prefs.accent = k;
                    gui_refresh_theme();
                }
            } else if (y >= 262 && y < 288) {
                gui_prefs.color_buttons = i == 1;
            } else if (y >= 316 && y < 342) {
                gui_prefs.anim = i == 0;
            }
        } else if (st->page == 1 && y >= 80 && y < 106) {
            gui_prefs.ptr_speed = (x - X0) / (SEGW / 3);
        } else if (st->page == 2 && y >= 212 && y < 238) {
            gui_prefs.pixfmt = (x - X0) / (SEGW / 3);
            gfx_set_pixel_format(gui_prefs.pixfmt);
        }
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

    if (st && st->page == 3)
        gui_redraw();                                /* keep the device list fresh */
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
    .name = "Settings", .title = "Settings", .cw = 520, .ch = 360, .single = true, .dock = 50,
    .open = settings_open, .draw = settings_draw, .click = settings_click, .key = settings_key,
    .tick = settings_tick, .icon = settings_icon,
};
GUI_APP(settings_app);
