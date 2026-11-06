/* notepad.c - a small text editor. Ctrl+S saves. */
#include "guiapi.h"
#include "uifont.h"
#include "input.h"
#include "vfs.h"
#include "heap.h"
#include "kerror.h"
#include "kprintf.h"
#include "kstring.h"
#include "sys.h"

#define CAP 16384

struct note {
    char *text;
    int len, cur, top_line;
    char file[VFS_PATH_MAX];
    char status[56];
    bool dirty, named;                  /* unsaved changes / the file has a name on disk */
    int pending;                        /* what to do once the question about saving is answered */
};

enum { PEND_NONE, PEND_CLOSE, PEND_NEW, PEND_OPEN };
enum { D_SAVEAS = 1, D_OPEN, D_GOTO, D_ASK };
enum { C_NEW = 1, C_OPEN, C_SAVE, C_SAVEAS, C_CLOSE, C_CUT, C_COPY, C_PASTE, C_DELLINE, C_GOTO };

static const struct gui_menu_item file_items[] = {
    { "New", C_NEW }, { "Open...", C_OPEN }, { "Save", C_SAVE }, { "Save As...", C_SAVEAS }, { "Close Window", C_CLOSE },
};
static const struct gui_menu_item edit_items[] = {
    { "Cut Line", C_CUT }, { "Copy Line", C_COPY }, { "Paste", C_PASTE }, { "Delete Line", C_DELLINE }, { "Go to Line...", C_GOTO },
};
static const struct gui_menu menus[] = {
    { "File", file_items, (int)ARRAY_SIZE(file_items) },
    { "Edit", edit_items, (int)ARRAY_SIZE(edit_items) },
};

static const char *base_name(const struct note *n)
{
    if (!n->named)
        return "Untitled";
    const char *b = strrchr(n->file, '/');
    return b ? b + 1 : n->file;
}

static void update_title(struct gui_window *w, const struct note *n)
{
    char t[48];

    ksnprintf(t, sizeof t, "%s%s", base_name(n), n->dirty ? " - Edited" : "");
    gui_set_title(w, t);
}

static void default_dir(char *out, size_t n)
{
    ksnprintf(out, n, "%s/Documents/", users_current() ? users_home() : "/tmp");
}

static void cursor_pos(const struct note *n, int *line, int *col)
{
    int l = 0, c = 0;

    for (int i = 0; i < n->cur; i++) {
        if (n->text[i] == '\n') { l++; c = 0; } else c++;
    }
    *line = l;
    *col = c;
}

static int line_start(const struct note *n, int line)
{
    int i = 0;

    while (line > 0 && i < n->len)
        if (n->text[i++] == '\n')
            line--;
    return i;
}

static int line_len(const struct note *n, int start)
{
    int c = 0;

    while (start + c < n->len && n->text[start + c] != '\n')
        c++;
    return c;
}

static void insert(struct note *n, char c)
{
    if (n->len >= CAP - 1)
        return;
    memmove(n->text + n->cur + 1, n->text + n->cur, (size_t)(n->len - n->cur));
    n->text[n->cur++] = c;
    n->len++;
}

static void notepad_open(struct gui_window *w)
{
    struct note *n = kzalloc(sizeof *n);

    if (n)
        n->text = kzalloc(CAP);
    if (!n || !n->text) {
        if (n)
            kfree(n->text);
        kfree(n);
        gui_close(w);
        return;
    }
    strcpy(n->status, "New file. Ctrl+S saves it.");
    w->priv = n;
    gui_set_title(w, "Untitled");
}

static void notepad_close(struct gui_window *w)
{
    struct note *n = w->priv;

    if (n)
        kfree(n->text);
    kfree(n);
    w->priv = NULL;
}

static void notepad_load(struct gui_window *w, const char *path)
{
    struct note *n;

    if (!w || !(n = w->priv))
        return;
    ssize_t r = vfs_read_file(path, n->text, CAP - 1);
    bool ok = r >= 0;
    if (!ok) {
        ksnprintf(n->status, sizeof n->status, "Cannot read: %s", kstrerror((int)r));
        gui_alert(w, "Could not open", kstrerror((int)r));
        return;
    }
    strlcpy(n->file, path, sizeof n->file);
    strcpy(n->status, "Opened. Ctrl+S saves.");
    n->len = (int)r;
    n->cur = 0;
    n->top_line = 0;
    n->named = true;
    n->dirty = false;
    for (int i = 0; i < n->len; i++)
        if ((uint8_t)n->text[i] > 126 || ((uint8_t)n->text[i] < 32 && n->text[i] != '\n'))
            n->text[i] = '.';
    update_title(w, n);
}

void notepad_open_file(const char *path)
{
    notepad_load(gui_open_named("Notepad"), path);
}

/* Write the file; returns true when it worked. */
static bool save_to(struct gui_window *w, struct note *n, const char *path)
{
    int r = vfs_write_file(path, n->text, (size_t)n->len);

    if (r < 0) {
        ksnprintf(n->status, sizeof n->status, "Save failed: %s", kstrerror(r));
        gui_alert(w, "Could not save", kstrerror(r));
        return false;
    }
    strlcpy(n->file, path, sizeof n->file);
    n->named = true;
    n->dirty = false;
    ksnprintf(n->status, sizeof n->status, "Saved %d bytes", n->len);
    update_title(w, n);
    return true;
}

static void ask_save_as(struct gui_window *w, struct note *n)
{
    char d[VFS_PATH_MAX];

    if (n->named)
        strlcpy(d, n->file, sizeof d);
    else {
        char dir[VFS_PATH_MAX];
        default_dir(dir, sizeof dir);
        ksnprintf(d, sizeof d, "%sUntitled.txt", dir);
    }
    gui_prompt(w, "Save as (path)", d, D_SAVEAS);
}

static void do_new(struct gui_window *w, struct note *n)
{
    n->len = n->cur = n->top_line = 0;
    n->text[0] = 0;
    n->named = n->dirty = false;
    n->file[0] = 0;
    strcpy(n->status, "New file. Ctrl+S saves it.");
    update_title(w, n);
}

static void do_open_prompt(struct gui_window *w)
{
    char d[VFS_PATH_MAX];

    default_dir(d, sizeof d);
    gui_prompt(w, "Open file (path)", d, D_OPEN);
}

/* Run what was waiting for the save question */
static void run_pending(struct gui_window *w, struct note *n)
{
    int p = n->pending;

    n->pending = PEND_NONE;
    if (p == PEND_CLOSE) {
        w->force_close = true;
        gui_close(w);
    } else if (p == PEND_NEW)
        do_new(w, n);
    else if (p == PEND_OPEN)
        do_open_prompt(w);
}

/* "Do you want to save?" when there are unsaved changes. Returns true if it is fine to go on right now. */
static bool check_unsaved(struct gui_window *w, struct note *n, int pend)
{
    char m[96];

    if (!n->dirty)
        return true;
    n->pending = pend;
    ksnprintf(m, sizeof m, "Save the changes to \"%s\" before you continue?", base_name(n));
    gui_ask_save(w, "Unsaved changes", m, D_ASK);
    return false;
}

static bool notepad_close_request(struct gui_window *w)
{
    struct note *n = w->priv;

    return !n || check_unsaved(w, n, PEND_CLOSE);
}

static void do_save(struct gui_window *w, struct note *n)
{
    if (!n->named)
        ask_save_as(w, n);
    else
        save_to(w, n, n->file);
}

static void notepad_dialog(struct gui_window *w, int id, bool ok, const char *text)
{
    struct note *n = w->priv;

    if (!n)
        return;
    switch (id) {
    case D_ASK:
        if (ok) {                                           /* Save */
            if (n->named) {
                if (save_to(w, n, n->file))
                    run_pending(w, n);
                else
                    n->pending = PEND_NONE;
            } else
                ask_save_as(w, n);                          /* then continues in D_SAVEAS */
        } else if (!strcmp(text, "discard")) {              /* Don't Save */
            n->dirty = false;
            run_pending(w, n);
        } else
            n->pending = PEND_NONE;                         /* Cancel */
        break;
    case D_SAVEAS:
        if (!ok) {
            n->pending = PEND_NONE;
        } else if (!text[0]) {
            gui_alert(w, "Save", "Please type a file name.");
            n->pending = PEND_NONE;
        } else if (save_to(w, n, text))
            run_pending(w, n);
        else
            n->pending = PEND_NONE;
        break;
    case D_OPEN:
        if (ok && text[0])
            notepad_load(w, text);
        break;
    case D_GOTO:
        if (ok) {
            int line = 0;
            for (const char *c = text; *c >= '0' && *c <= '9'; c++)
                line = line * 10 + (*c - '0');
            n->cur = line_start(n, MAX(0, line - 1));
            if (n->cur > n->len) n->cur = n->len;
            n->top_line = MAX(0, line - 5);
        }
        break;
    }
    gui_redraw();
}

static void notepad_command(struct gui_window *w, int id)
{
    struct note *n = w->priv;
    int line, col;

    if (!n)
        return;
    cursor_pos(n, &line, &col);
    int st = line_start(n, line), len = line_len(n, st);
    switch (id) {
    case C_NEW:
        if (check_unsaved(w, n, PEND_NEW))
            do_new(w, n);
        break;
    case C_OPEN:
        if (check_unsaved(w, n, PEND_OPEN))
            do_open_prompt(w);
        break;
    case C_SAVE: do_save(w, n); break;
    case C_SAVEAS: ask_save_as(w, n); break;
    case C_CLOSE: gui_close(w); break;
    case C_CUT:
    case C_COPY:
    case C_DELLINE:
        if (id != C_DELLINE) {
            char tmp[256];
            size_t l = MIN((size_t)len, sizeof tmp - 2);
            memcpy(tmp, n->text + st, l);
            tmp[l] = 0;
            gui_clip_set(GUI_CLIP_TEXT, tmp);
        }
        if (id != C_COPY) {                                  /* cut and delete remove the line */
            int end = st + len + (st + len < n->len ? 1 : 0);
            memmove(n->text + st, n->text + end, (size_t)(n->len - end));
            n->len -= end - st;
            n->cur = MIN(st, n->len);
            n->dirty = true;
        }
        break;
    case C_PASTE: {
        const char *c = gui_clip_kind() == GUI_CLIP_TEXT ? gui_clip_get() : "";
        for (; *c; c++)
            insert(n, *c == '\n' || (*c >= 32 && *c < 127) ? *c : ' ');
        n->dirty = true;
        break;
    }
    case C_GOTO: gui_prompt(w, "Go to line", "1", D_GOTO); break;
    }
    if (n->dirty)
        update_title(w, n);
    gui_redraw();
}

static void notepad_key(struct gui_window *w, int key, int mods)
{
    struct note *n = w->priv;
    int line, col;

    cursor_pos(n, &line, &col);
    if (key == 19 || ((mods & MOD_CTRL) && (key == 's' || key == 'S'))) {
        do_save(w, n);
        gui_redraw();
        return;
    }
    if (key == 15 || ((mods & MOD_CTRL) && (key == 'o' || key == 'O'))) {
        notepad_command(w, C_OPEN);
        return;
    }
    if (key == 14 || ((mods & MOD_CTRL) && (key == 'n' || key == 'N'))) {
        notepad_command(w, C_NEW);
        return;
    }
    switch (key) {
    case KEY_LEFT:  if (n->cur > 0) n->cur--; break;
    case KEY_RIGHT: if (n->cur < n->len) n->cur++; break;
    case KEY_HOME:  n->cur = line_start(n, line); break;
    case KEY_END: { int st = line_start(n, line); n->cur = st + line_len(n, st); break; }
    case KEY_UP:
        if (line > 0) {
            int st = line_start(n, line - 1);
            n->cur = st + MIN(col, line_len(n, st));
        }
        break;
    case KEY_DOWN: {
        int cs = line_start(n, line), st = line_start(n, line + 1);
        if (st > cs && st <= n->len)
            n->cur = st + MIN(col, line_len(n, st));
        break;
    }
    case '\b': case 127:
        if (n->cur > 0) {
            n->dirty = true;
            memmove(n->text + n->cur - 1, n->text + n->cur, (size_t)(n->len - n->cur));
            n->cur--;
            n->len--;
        }
        break;
    case KEY_DELETE:
        if (n->cur < n->len) {
            n->dirty = true;
            memmove(n->text + n->cur, n->text + n->cur + 1, (size_t)(n->len - n->cur - 1));
            n->len--;
        }
        break;
    case '\n': insert(n, '\n'); n->dirty = true; break;
    case '\t': for (int i = 0; i < 4; i++) insert(n, ' '); n->dirty = true; break;
    default:
        if (key >= 32 && key < 127) {
            insert(n, (char)key);
            n->dirty = true;
        }
        break;
    }
    if (n->dirty)
        update_title(w, n);
    cursor_pos(n, &line, &col);
    int rows = (w->ch - 22) / 16;
    if (line < n->top_line) n->top_line = line;
    if (line >= n->top_line + rows) n->top_line = line - rows + 1;
    gui_redraw();
}

static void notepad_scroll(struct gui_window *w, int lines)
{
    struct note *n = w->priv;
    int total = 1;

    if (!n)
        return;
    for (int i = 0; i < n->len; i++)
        if (n->text[i] == '\n')
            total++;
    n->top_line = MAX(0, MIN(total - 1, n->top_line + lines));
    gui_redraw();
}

static void notepad_draw(struct gui_window *w)
{
    struct note *n = w->priv;
    struct gfx_surface *s = w->surf;
    int rows = (w->ch - 22) / 16, cols = (w->cw - 12) / 8, line, col;

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, gui_th->win);
    if (!n)
        return;
    cursor_pos(n, &line, &col);
    int st = line_start(n, n->top_line);
    for (int r = 0; r < rows && st <= n->len; r++) {
        int len = line_len(n, st);
        for (int c = 0; c < len && c < cols; c++)
        {
            char ch = n->text[st + c];
            if (ch >= 33 && ch < 127)
                ui_mono_char(s, 6 + c * 8, 4 + r * 16, ch, gui_th->text, false);
            else if (ch != ' ')
                gfx_char_tall(s, 6 + c * 8, 4 + r * 16, ch, gui_th->text, GFX_TRANSPARENT);
        }
        if (n->top_line + r == line && col <= cols)
            gfx_fill_rect(s, 6 + col * 8, 4 + r * 16, 2, 16, gui_th->accent);
        st += len + 1;
    }
    gfx_fill_rect(s, 0, w->ch - 20, w->cw, 20, gui_th->side);
    gfx_hline(s, 0, w->ch - 20, w->cw, gui_th->line);
    char buf[110];
    ksnprintf(buf, sizeof buf, "Ln %d, Col %d    %s", line + 1, col + 1, n->status);
    gfx_text(s, 10, w->ch - 14, buf, gui_th->dim, GFX_TRANSPARENT, 1);
}

static void notepad_icon(struct gfx_surface *s, int x, int y, int sz, uint32_t g, uint32_t tile)
{
#define P(f) ((sz * (f)) / 100)
    gfx_fill_round_rect(s, x + P(26), y + P(16), P(48), P(68), P(6), g);
    for (int i = 0; i < 4; i++)
        gfx_fill_rect(s, x + P(33), y + P(30) + i * P(13), P(34), MAX(P(4), 2), tile);
#undef P
}

static const struct gui_app notepad_app = {
    .name = "Notepad", .title = "Notepad - new file", .cw = 560, .ch = 360, .dock = 30,
    .open = notepad_open, .close = notepad_close, .draw = notepad_draw, .key = notepad_key, .scroll = notepad_scroll,
    .menus = menus, .nmenus = (int)ARRAY_SIZE(menus), .command = notepad_command, .dialog = notepad_dialog,
    .close_request = notepad_close_request,
    .icon = notepad_icon,
};
GUI_APP(notepad_app);
