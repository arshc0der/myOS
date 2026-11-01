/*
 * hello_app.c - YOUR first desktop application. Build, boot the graphical
 * desktop and look for "Hello" in the dock. Click inside the window or type.
 * Copy this file to start a new app; the only things you must provide are
 * the draw function and the GUI_APP(...) line at the bottom.
 */
#include "guiapi.h"
#include "input.h"
#include "kprintf.h"

static int clicks;
static char last_key[16] = "(none)";

static void hello_draw(struct gui_window *w)
{
    char line[64];

    gfx_fill_rect(w->surf, 0, 0, w->cw, w->ch, gui_th->win);
    gfx_text(w->surf, 20, 20, "Hello from my own app!", gui_th->text, GFX_TRANSPARENT, 2);
    ksnprintf(line, sizeof line, "Clicks: %d", clicks);
    gfx_text(w->surf, 20, 70, line, gui_th->text, GFX_TRANSPARENT, 1);
    ksnprintf(line, sizeof line, "Last key: %s", last_key);
    gfx_text(w->surf, 20, 90, line, gui_th->text, GFX_TRANSPARENT, 1);
    gui_button(w->surf, 20, 130, 120, 30, "Click me", clicks & 1);
}

static void hello_click(struct gui_window *w, int x, int y, int buttons)
{
    (void)w; (void)x; (void)y; (void)buttons;
    clicks++;
    gui_redraw();
}

static void hello_key(struct gui_window *w, int key, int mods)
{
    (void)w; (void)mods;
    ksnprintf(last_key, sizeof last_key, key < 128 ? "%c" : "code %d", key);
    gui_redraw();
}

static const struct gui_app hello_app = {
    .name = "Hello", .title = "Hello", .cw = 320, .ch = 200, .dock = 90,
    .draw = hello_draw, .click = hello_click, .key = hello_key,
};
GUI_APP(hello_app);
