/* about.c - "About MyOS". */
#include "guiapi.h"
#include "version.h"

static void about_draw(struct gui_window *w)
{
    struct gfx_surface *s = w->surf;
    int cx = w->cw / 2;

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, gui_th->win);
    gfx_draw_logo(s, cx - 40, 24, 80);
    gfx_text(s, cx - gfx_text_width("MyOS", 3) / 2, 116, "MyOS", gui_th->text, GFX_TRANSPARENT, 3);
    gui_label_center(s, 0, 150, w->cw, "Version " MYOS_VERSION " \"" MYOS_CODENAME "\"", gui_th->dim);
    gui_label_center(s, 0, 180, w->cw, "A 32-bit operating system written", gui_th->text);
    gui_label_center(s, 0, 194, w->cw, "from scratch in C and assembly.", gui_th->text);
    gui_label_center(s, 0, 226, w->cw, "github.com/arshc0der/myOS", gui_th->dim);
}

static const struct gui_app about_app = {
    .name = "About", .title = "About MyOS", .cw = 340, .ch = 260, .single = true, .dock = -1,
    .draw = about_draw,
};
GUI_APP(about_app);
