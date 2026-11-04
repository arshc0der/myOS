# GUI API - writing your own desktop application

The desktop core (`src/gui/gui.c`) manages windows, the menu bar, the dock, dialogs
and input. Every application is one file that describes itself with a `struct gui_app`
(`include/guiapi.h`) and registers with `GUI_APP(...)`. No change to the core is needed.

Put your file in `src/user/` (see `src/user/hello_app.c`) or `src/gui/apps/`; the
Makefile picks up every `.c` file automatically.

## Smallest app

```c
#include "guiapi.h"

static void hello_draw(struct gui_window *w)
{
    gfx_fill_rect(w->surf, 0, 0, w->cw, w->ch, gui_th->win);
    gfx_text(w->surf, 20, 20, "Hello!", gui_th->text, GFX_TRANSPARENT, 2);
}

static const struct gui_app hello = {
    .name = "Hello", .cw = 300, .ch = 200, .dock = 90, .draw = hello_draw,
};
GUI_APP(hello);
```

## Callbacks (all optional except `draw`)

| Field | When |
|-------|------|
| `open(w)` / `close(w)` | window created / going away. Keep your data in `w->priv`. |
| `draw(w)` | paint into `w->surf` (size `w->cw` x `w->ch`) |
| `key(w, key, mods)` | keyboard, when your window is in front. `key` is ASCII or `KEY_*`; Ctrl+C arrives as 3, Ctrl+X 24, Ctrl+V 22. |
| `click(w, x, y, buttons)` | mouse press inside the window. `buttons`: `GUI_BTN_LEFT`, `GUI_BTN_RIGHT`, plus `GUI_BTN_DOUBLE` on a double click |
| `tick(w)` | about 10 times a second (animations, refresh) |
| `command(w, id)` | a menu or popup item with that id was chosen |
| `dialog(w, id, ok, text)` | answer of `gui_prompt` / `gui_confirm` |
| `icon(...)` | draw your dock icon (optional) |

Other fields: `.single` (only one window), `.dock` (position in the dock, negative = not in
the dock), `.dark_tile`, `.size` (choose the window size at run time).

## Menus in the menu bar

```c
static const struct gui_menu_item file_items[] = { { "New", 1 }, { "-", 0 }, { "Close", 2 } };
static const struct gui_menu menus[] = { { "File", file_items, 3 } };
// in the app: .menus = menus, .nmenus = 1, .command = my_command
```

A label of `"-"` is a separator; a label starting with `~` is dimmed and cannot be chosen.

## Services (`guiapi.h`)

| Function | Purpose |
|----------|---------|
| `gui_popup(w, x, y, items, n)` | right-click menu at client position x,y |
| `gui_prompt(w, title, initial, id)` | ask for a line of text |
| `gui_confirm(w, title, message, id)` | OK / Cancel question |
| `gui_alert(w, title, message)` | information box |
| `gui_clip_set(kind, text)`, `gui_clip_get()`, `gui_clip_kind()` | clipboard shared by all apps |
| `gui_open_named("Files")`, `gui_close(w)`, `gui_set_title(w, t)`, `gui_minimize(w)` | window control |
| `gui_set_resolution(w, h)`, `gui_mode_count()`, `gui_mode_at(i, &w, &h)` | screen size (virtual machines) |
| `gui_redraw()` | request a new frame |

## Drawing helpers

`gfx.h`: rectangles, circles, rounded rectangles, lines, gradients, text (`gfx_text`), blit, alpha blend.
`guiapi.h` (widgets): `gui_button`, `gui_segment`, `gui_sidebar_row`, `gui_textfield`,
`gui_toolbar_button`, `gui_scrollbar`, `gui_file_icon`, `gui_hit`.
Always use the theme colours (`gui_th->text`, `->win`, `->accent`, ...) so your app follows
the light/dark theme and the accent colour chosen in Settings.

## Files and tasks

Use `vfs.h` for files and `fileops.h` for copy / move / delete / rename (see `MYFS.md`).
`sched.h` lists and controls tasks (`sched_for_each`, `task_kill`); the Task Manager
(`src/gui/apps/taskmgr.c`) is a complete example. `src/gui/apps/files.c` shows menus, popups,
dialogs and the clipboard together.

## Added in 2.4.0

| Function | Purpose |
|----------|---------|
| `gui_maximize(w)` | toggle maximize / restore (green button does the same) |
| `gui_resize(w, cw, ch)` | change the client size of a window (the surface is re-created) |
| `gui_set_wallpaper(path)` | picture as desktop background |
| `gui_reload_icons()` | re-read the custom dock icon pictures |
| `gui_fps()` | frames drawn in the last second |
| `photos_open_file(path)` | open a picture in Photos |
| `gui_login()` | the Welcome / Log in screen (called once at start) |

`gui_prefs` gained `dock_colorful`, `dock_size`, `show_fps`. Pictures: `image.h` (see `IMAGES.md`).
Users, disks and time zone: `sys.h` (see `SYSTEM.md`).
A window's size can change at any time (maximize, resolution change): always draw from `w->cw` and `w->ch`.
