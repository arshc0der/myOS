/*
 * login.c - first-boot account setup and the login screen.
 *
 * No account yet: "Welcome" asks for a user name and password and creates the
 * first (administrator) account and its home folder /Users/<name>.
 * Otherwise: pick a user with Up / Down (or Tab) and type the password.
 * Keyboard driven: Tab switches field, Enter confirms.
 */
#include "guiapi.h"
#include "sys.h"
#include "input.h"
#include "timer.h"
#include "kstring.h"
#include "kprintf.h"
#include "kerror.h"
#include "multiboot.h"

#define FIELD_MAX 24

static void card(struct gfx_surface *s, int w, int h, int cw, int ch, int *cx, int *cy)
{
    uint32_t top = gui_th->bg_top, bot = gui_th->bg_bot;
    gfx_gradient_v(s, 0, 0, w, h, top, bot);
    *cx = (w - cw) / 2;
    *cy = (h - ch) / 2;
    gfx_fill_round_rect(s, *cx + 3, *cy + 6, cw, ch, 16, gfx_blend(bot, GFX_BLACK, 40));
    gfx_fill_round_rect(s, *cx - 1, *cy - 1, cw + 2, ch + 2, 17, gui_th->line);
    gfx_fill_round_rect(s, *cx, *cy, cw, ch, 16, gui_th->win);
    gfx_draw_logo(s, *cx + cw / 2 - 28, *cy + 18, 56);
}

static void mask(char *out, const char *pw)
{
    int n = (int)strlen(pw);
    for (int i = 0; i < n && i < FIELD_MAX; i++)
        out[i] = '*';
    out[n < FIELD_MAX ? n : FIELD_MAX] = 0;
}

static void edit(char *buf, int32_t key, bool name)
{
    int n = (int)strlen(buf);
    if (key == '\b' || key == 127) {
        if (n)
            buf[n - 1] = 0;
    } else if (key >= 32 && key < 127 && n < FIELD_MAX - 1) {
        if (name && !((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || key == '_' || key == '-' || key == '.'))
            return;
        buf[n] = (char)key;
        buf[n + 1] = 0;
    }
}

void gui_login(void)
{
    struct gfx_surface *s = gfx_screen();
    int w = gfx_width(), h = gfx_height();
    char name[FIELD_MAX] = "", pw[FIELD_MAX] = "", pw2[FIELD_MAX] = "";
    int field = 0, sel = 0;
    const char *msg = "";
    bool setup = users_count() == 0;
    bool auto_in = boot_has_arg("guitest");

    if (auto_in && setup) {                     /* test mode: create a demo account */
        users_add("demo", "", true);
        setup = false;
    }
    if (auto_in) {
        users_login(users_get(0)->name);
        return;
    }
    for (;;) {
        int cx, cy, cw = 380, ch = setup ? 330 : 120 + 36 * (users_count() > 4 ? 4 : users_count()) + 100;
        char hid[FIELD_MAX + 1];

        card(s, w, h, cw, ch, &cx, &cy);
        if (setup) {
            gui_label_center(s, cx, cy + 86, cw, "Welcome to MyOS", gui_th->text);
            gfx_text(s, cx + 30, cy + 118, "Create your account", gui_th->dim, GFX_TRANSPARENT, 1);
            gfx_text(s, cx + 30, cy + 142, "User name", gui_th->dim, GFX_TRANSPARENT, 1);
            gui_textfield(s, cx + 30, cy + 158, cw - 60, name, field == 0);
            gfx_text(s, cx + 30, cy + 192, "Password (may be empty)", gui_th->dim, GFX_TRANSPARENT, 1);
            mask(hid, pw);
            gui_textfield(s, cx + 30, cy + 208, cw - 60, hid, field == 1);
            gfx_text(s, cx + 30, cy + 242, "Repeat password", gui_th->dim, GFX_TRANSPARENT, 1);
            mask(hid, pw2);
            gui_textfield(s, cx + 30, cy + 258, cw - 60, hid, field == 2);
            gui_label_center(s, cx, cy + 296, cw, msg[0] ? msg : "Tab: next field    Enter: create", msg[0] ? GFX_RGB(220, 60, 50) : gui_th->dim);
            if (!sys_users_persistent())
                gui_label_center(s, cx, cy + 312, cw, "No MyFS disk found: the account lasts until reboot", gui_th->dim);
        } else {
            gui_label_center(s, cx, cy + 86, cw, "Log in", gui_th->text);
            int n = users_count();
            for (int i = 0; i < n && i < 4; i++) {
                char row[40];
                ksnprintf(row, sizeof row, "%s%s", users_get(i)->name, users_get(i)->admin ? "  (admin)" : "");
                gui_sidebar_row(s, cx + 30, cy + 120 + 36 * i, cw - 60, row, i == sel);
            }
            int py = cy + 120 + 36 * (n > 4 ? 4 : n) + 8;
            gfx_text(s, cx + 30, py, "Password", gui_th->dim, GFX_TRANSPARENT, 1);
            mask(hid, pw);
            gui_textfield(s, cx + 30, py + 16, cw - 60, hid, true);
            gui_label_center(s, cx, py + 56, cw, msg[0] ? msg : "Up / Down: choose user    Enter: log in", msg[0] ? GFX_RGB(220, 60, 50) : gui_th->dim);
        }
        gfx_flush_all();

        struct input_event ev;
        if (input_wait(&ev, 200) != 0 || ev.type != INPUT_KEY)
            continue;
        msg = "";
        if (setup) {
            char *f = field == 0 ? name : field == 1 ? pw : pw2;
            if (ev.key == '\t' || ev.key == KEY_DOWN)
                field = (field + 1) % 3;
            else if (ev.key == KEY_UP)
                field = (field + 2) % 3;
            else if (ev.key == '\n') {
                if (field < 2) {
                    field++;
                } else if (!name[0]) {
                    msg = "Please enter a user name";
                    field = 0;
                } else if (strcmp(pw, pw2) != 0) {
                    msg = "The passwords do not match";
                    field = 1;
                } else {
                    int r = users_add(name, pw, true);
                    if (r < 0)
                        msg = "Could not create the account";
                    else {
                        users_login(name);
                        return;
                    }
                }
            } else
                edit(f, ev.key, field == 0);
        } else {
            int n = users_count();
            if (ev.key == KEY_UP && sel > 0)
                sel--;
            else if ((ev.key == KEY_DOWN || ev.key == '\t') && sel + 1 < n)
                sel++;
            else if (ev.key == '\n') {
                if (users_check(users_get(sel)->name, pw)) {
                    users_login(users_get(sel)->name);
                    return;
                }
                msg = "Wrong password";
                pw[0] = 0;
            } else
                edit(pw, ev.key, false);
        }
    }
}
