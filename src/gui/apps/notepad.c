/* notepad.c - a small text editor. Ctrl+S saves. */
#include "guiapi.h"
#include "input.h"
#include "vfs.h"
#include "heap.h"
#include "kerror.h"
#include "kprintf.h"
#include "kstring.h"

#define CAP 16384

struct note {
    char *text;
    int len, cur, top_line;
    char file[VFS_PATH_MAX];
    char status[56];
};

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
    strcpy(n->file, "/tmp/notes.txt");
    strcpy(n->status, "New file. Ctrl+S saves to /tmp/notes.txt");
    w->priv = n;
}

static void notepad_close(struct gui_window *w)
{
    struct note *n = w->priv;

    if (n)
        kfree(n->text);
    kfree(n);
    w->priv = NULL;
}

void notepad_open_file(const char *path)
{
    struct gui_window *w = gui_open_named("Notepad");
    struct note *n;

    if (!w || !(n = w->priv))
        return;
    strlcpy(n->file, path, sizeof n->file);
    ssize_t r = vfs_read_file(path, n->text, CAP - 1);
    if (r < 0) {
        ksnprintf(n->status, sizeof n->status, "Cannot read: %s", kstrerror((int)r));
        r = 0;
    } else {
        strcpy(n->status, "Opened. Ctrl+S saves.");
    }
    n->len = (int)r;
    n->cur = 0;
    for (int i = 0; i < n->len; i++)
        if ((uint8_t)n->text[i] > 126 || ((uint8_t)n->text[i] < 32 && n->text[i] != '\n'))
            n->text[i] = '.';
    const char *base = strrchr(path, '/');
    char t[40];
    ksnprintf(t, sizeof t, "%s", base ? base + 1 : path);
    gui_set_title(w, t);
}

static void notepad_key(struct gui_window *w, int key, int mods)
{
    struct note *n = w->priv;
    int line, col;

    cursor_pos(n, &line, &col);
    if (key == 19 || ((mods & MOD_CTRL) && (key == 's' || key == 'S'))) {
        int r = vfs_write_file(n->file, n->text, (size_t)n->len);
        if (r < 0)
            ksnprintf(n->status, sizeof n->status, "Save failed: %s", kstrerror(r));
        else
            ksnprintf(n->status, sizeof n->status, "Saved %d bytes to %s", n->len, n->file);
        gui_redraw();
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
            memmove(n->text + n->cur - 1, n->text + n->cur, (size_t)(n->len - n->cur));
            n->cur--;
            n->len--;
        }
        break;
    case KEY_DELETE:
        if (n->cur < n->len) {
            memmove(n->text + n->cur, n->text + n->cur + 1, (size_t)(n->len - n->cur - 1));
            n->len--;
        }
        break;
    case '\n': insert(n, '\n'); break;
    case '\t': for (int i = 0; i < 4; i++) insert(n, ' '); break;
    default:
        if (key >= 32 && key < 127)
            insert(n, (char)key);
        break;
    }
    cursor_pos(n, &line, &col);
    int rows = (w->ch - 22) / 16;
    if (line < n->top_line) n->top_line = line;
    if (line >= n->top_line + rows) n->top_line = line - rows + 1;
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
            gfx_char_tall(s, 6 + c * 8, 4 + r * 16, n->text[st + c], gui_th->text, GFX_TRANSPARENT);
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
    .open = notepad_open, .close = notepad_close, .draw = notepad_draw, .key = notepad_key,
    .icon = notepad_icon,
};
GUI_APP(notepad_app);
