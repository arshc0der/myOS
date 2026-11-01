/* widgets.c - themes and small drawing helpers shared by the desktop and its apps. */
#include "guiapi.h"
#include "kstring.h"

const struct gui_theme gui_theme_light = {
    .bg_top = GFX_RGB(246, 246, 248), .bg_bot = GFX_RGB(220, 222, 228),
    .win = GFX_RGB(255, 255, 255), .side = GFX_RGB(242, 242, 245),
    .title = GFX_RGB(246, 246, 248), .title_off = GFX_RGB(250, 250, 251),
    .text = GFX_RGB(28, 28, 30), .dim = GFX_RGB(130, 130, 138), .line = GFX_RGB(222, 222, 227),
    .menu = GFX_RGB(250, 250, 252),
    .sel = GFX_RGB(222, 224, 232), .sel_text = GFX_RGB(28, 28, 30),
    .accent = GFX_RGB(28, 28, 30), .accent_fg = GFX_RGB(255, 255, 255),
    .tile = GFX_RGB(255, 255, 255), .tile_edge = GFX_RGB(196, 198, 206), .glyph = GFX_RGB(28, 28, 30),
    .tl_mono = { GFX_RGB(70, 70, 76), GFX_RGB(150, 150, 158), GFX_RGB(200, 200, 206) },
};

const struct gui_theme gui_theme_dark = {
    .bg_top = GFX_RGB(44, 44, 48), .bg_bot = GFX_RGB(8, 8, 10),
    .win = GFX_RGB(30, 30, 32), .side = GFX_RGB(38, 38, 41),
    .title = GFX_RGB(52, 52, 56), .title_off = GFX_RGB(40, 40, 43),
    .text = GFX_RGB(240, 240, 245), .dim = GFX_RGB(150, 150, 158), .line = GFX_RGB(64, 64, 70),
    .menu = GFX_RGB(34, 34, 37),
    .sel = GFX_RGB(70, 70, 76), .sel_text = GFX_RGB(255, 255, 255),
    .accent = GFX_RGB(245, 245, 247), .accent_fg = GFX_RGB(20, 20, 22),
    .tile = GFX_RGB(58, 58, 62), .tile_edge = GFX_RGB(100, 100, 108), .glyph = GFX_RGB(245, 245, 248),
    .tl_mono = { GFX_RGB(235, 235, 240), GFX_RGB(160, 160, 168), GFX_RGB(110, 110, 118) },
};

/* The theme in use: a copy of the light or dark theme with the accent colour applied. */
static struct gui_theme cur_theme;
const struct gui_theme *gui_th = &cur_theme;
struct gui_prefs gui_prefs = { .dark = false, .anim = true, .color_buttons = true, .accent = 0, .ptr_speed = 1 };

const char *const gui_accent_name[GUI_ACCENTS] = {
    "Blue", "Purple", "Pink", "Red", "Orange", "Green", "Graphite"
};

static const uint32_t accent_light[GUI_ACCENTS] = {
    GFX_RGB(0, 122, 255), GFX_RGB(175, 82, 222), GFX_RGB(255, 45, 85), GFX_RGB(255, 59, 48),
    GFX_RGB(255, 149, 0), GFX_RGB(40, 180, 70), GFX_RGB(28, 28, 30)
};
static const uint32_t accent_dark[GUI_ACCENTS] = {
    GFX_RGB(10, 132, 255), GFX_RGB(191, 90, 242), GFX_RGB(255, 55, 95), GFX_RGB(255, 69, 58),
    GFX_RGB(255, 159, 10), GFX_RGB(48, 209, 88), GFX_RGB(245, 245, 247)
};

uint32_t gui_accent_color(int i, bool dark)
{
    if (i < 0 || i >= GUI_ACCENTS)
        i = 0;
    return dark ? accent_dark[i] : accent_light[i];
}

void gui_theme_apply(void)
{
    int a = gui_prefs.accent;

    cur_theme = gui_prefs.dark ? gui_theme_dark : gui_theme_light;
    if (a < 0 || a >= GUI_ACCENTS)
        a = 0;
    if (a != GUI_ACCENTS - 1) {                         /* a colour: filled buttons, selections, wallpaper tint */
        uint32_t c = gui_accent_color(a, gui_prefs.dark);
        cur_theme.accent = c;
        cur_theme.accent_fg = GFX_RGB(255, 255, 255);
        cur_theme.sel = c;
        cur_theme.sel_text = GFX_RGB(255, 255, 255);
        cur_theme.bg_bot = gfx_blend(cur_theme.bg_bot, c, gui_prefs.dark ? 40 : 34);
    }
}

void gui_button(struct gfx_surface *s, int x, int y, int w, int h, const char *label, bool on)
{
    gfx_fill_round_rect(s, x, y, w, h, 7, on ? gui_th->accent : gui_th->line);
    if (!on)
        gfx_fill_round_rect(s, x + 1, y + 1, w - 2, h - 2, 6, gui_th->win);
    gfx_text(s, x + (w - gfx_text_width(label, 1)) / 2, y + (h - 8) / 2, label,
             on ? gui_th->accent_fg : gui_th->text, GFX_TRANSPARENT, 1);
}

/* macOS style segmented control: n equal segments, one active. */
void gui_segment(struct gfx_surface *s, int x, int y, int w, const char **labels, int n, int active)
{
    int seg = w / n;

    gfx_fill_round_rect(s, x, y, seg * n, 26, 7, gui_th->line);
    gfx_fill_round_rect(s, x + 1, y + 1, seg * n - 2, 24, 6, gui_th->side);
    for (int i = 0; i < n; i++) {
        if (i == active)
            gfx_fill_round_rect(s, x + i * seg + 2, y + 2, seg - 4, 22, 5, gui_th->accent);
        gfx_text(s, x + i * seg + (seg - gfx_text_width(labels[i], 1)) / 2, y + 9, labels[i],
                 i == active ? gui_th->accent_fg : gui_th->text, GFX_TRANSPARENT, 1);
    }
}

void gui_sidebar_row(struct gfx_surface *s, int x, int y, int w, const char *label, bool sel)
{
    if (sel)
        gfx_fill_round_rect(s, x + 6, y, w - 12, 24, 6, gui_th->sel);
    gfx_text(s, x + 16, y + 8, label, sel ? gui_th->sel_text : gui_th->text, GFX_TRANSPARENT, 1);
}

void gui_label_center(struct gfx_surface *s, int x, int y, int w, const char *text, uint32_t col)
{
    gfx_text(s, x + (w - gfx_text_width(text, 1)) / 2, y, text, col, GFX_TRANSPARENT, 1);
}
