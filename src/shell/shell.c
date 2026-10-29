/*
 * shell.c - the interactive command line.
 *
 * Features: line editing (arrows, Home/End, Delete), history (Up/Down),
 * Tab completion of commands and file names, quoting ("a b"), several
 * commands per line separated by ';', and output redirection: `cmd > file`
 * and `cmd >> file`.
 */
#include "shell.h"
#include "input.h"
#include "console.h"
#include "kprintf.h"
#include "kstring.h"
#include "kerror.h"
#include "heap.h"
#include "vfs.h"
#include "sched.h"
#include "version.h"

extern const struct shell_cmd __start_shell_cmds[];
extern const struct shell_cmd __stop_shell_cmds[];

#define LINE_MAX    256
#define HIST_MAX    16
#define MAX_ARGS    24

static char history[HIST_MAX][LINE_MAX];
static int hist_count, hist_next;
bool shell_exit_requested;
static int last_status;

/* ---------- command table ---------- */

const struct shell_cmd *shell_find(const char *name)
{
    for (const struct shell_cmd *c = __start_shell_cmds; c < __stop_shell_cmds; c++)
        if (strcmp(c->name, name) == 0)
            return c;
    return NULL;
}

void shell_for_each(void (*cb)(const struct shell_cmd *, void *), void *ctx)
{
    for (const struct shell_cmd *c = __start_shell_cmds; c < __stop_shell_cmds; c++)
        cb(c, ctx);
}

/* ---------- parsing ---------- */

/*
 * Split `line` in place into words. Handles "double quotes", 'single
 * quotes' and backslash. A word `>` or `>>` (or one starting with it) is
 * a redirection and is returned separately.
 */
static int parse(char *line, char **argv, int max, char **redir, bool *append)
{
    int argc = 0;
    char *p = line;

    *redir = NULL;
    *append = false;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        char *out = p, *start = p;
        char quote = 0;
        bool is_redir = false;

        if (*p == '>') {
            is_redir = true;
            p++;
            if (*p == '>') {
                *append = true;
                p++;
            }
            while (*p == ' ')
                p++;
            start = out = p;
        }
        while (*p && (quote || (*p != ' ' && *p != '\t'))) {
            if (quote) {
                if (*p == quote)
                    quote = 0;
                else
                    *out++ = *p;
            } else if (*p == '"' || *p == '\'') {
                quote = *p;
            } else if (*p == '\\' && p[1]) {
                *out++ = *++p;
            } else {
                *out++ = *p;
            }
            p++;
        }
        bool at_end = *p == '\0';
        *out = '\0';
        if (!at_end)
            p++;
        if (is_redir)
            *redir = start;
        else if (argc < max - 1)
            argv[argc++] = start;
    }
    argv[argc] = NULL;
    return argc;
}

/* ---------- running a command ---------- */

struct capture {
    char *buf;
    size_t len, cap;
    bool failed;
};

static void capture_char(char c, void *ctx)
{
    struct capture *cp = ctx;

    if (cp->failed)
        return;
    if (cp->len + 1 > cp->cap) {
        size_t ncap = cp->cap ? cp->cap * 2 : 256;
        char *nb = krealloc(cp->buf, ncap);
        if (!nb) {
            cp->failed = true;
            return;
        }
        cp->buf = nb;
        cp->cap = ncap;
    }
    cp->buf[cp->len++] = c;
}

static int run_one(char *cmdline)
{
    char *argv[MAX_ARGS];
    char *redir;
    bool append;
    int argc = parse(cmdline, argv, MAX_ARGS, &redir, &append);

    if (argc == 0)
        return 0;
    const struct shell_cmd *c = shell_find(argv[0]);
    if (!c) {
        console_set_color(CON_RED);
        kprintf("%s: command not found (try 'help')\n", argv[0]);
        console_set_color(CON_NORMAL);
        return 127;
    }

    int rc;
    if (redir && *redir) {
        struct capture cp = { NULL, 0, 0, false };
        void (*old_fn)(char, void *);
        void *old_ctx;
        console_get_capture(&old_fn, &old_ctx);
        console_set_capture(capture_char, &cp);
        rc = c->fn(argc, argv);
        console_set_capture(old_fn, old_ctx);
        int r = cp.failed ? -ENOMEM :
                append ? vfs_append_file(redir, cp.buf, cp.len)
                       : vfs_write_file(redir, cp.buf, cp.len);
        if (r < 0) {
            kprintf("%s: %s\n", redir, kstrerror(r));
            rc = 1;
        }
        kfree(cp.buf);
    } else {
        rc = c->fn(argc, argv);
    }
    return rc;
}

int shell_exec(const char *line)
{
    char buf[LINE_MAX];
    char *p;
    int rc = 0;

    strlcpy(buf, line, sizeof buf);
    p = buf;
    while (*p) {                                /* split on ';' outside quotes */
        char *start = p;
        char quote = 0;
        while (*p && (quote || *p != ';')) {
            if (quote && *p == quote)
                quote = 0;
            else if (!quote && (*p == '"' || *p == '\''))
                quote = *p;
            p++;
        }
        bool more = *p == ';';
        *p = '\0';
        rc = run_one(start);
        if (more)
            p++;
    }
    last_status = rc;
    return rc;
}

/* ---------- line editor ---------- */

static void show_prompt(void)
{
    console_set_color(CON_GREEN);
    kprintf("myos");
    console_set_color(CON_NORMAL);
    kprintf(":");
    console_set_color(CON_CYAN);
    kprintf("%s", vfs_getcwd());
    console_set_color(CON_NORMAL);
    kprintf("$ ");
}

/* Redraw from cursor position `pos` to the end, then put the cursor back. */
static void redraw_tail(const char *line, int pos, int len, int extra_blanks)
{
    for (int i = pos; i < len; i++)
        console_putc(line[i]);
    for (int i = 0; i < extra_blanks; i++)
        console_putc(' ');
    for (int i = 0; i < (len - pos) + extra_blanks; i++)
        console_putc('\b');
}

struct match_ctx {
    const char *prefix;
    size_t plen;
    char first[VFS_NAME_MAX + 1];
    int count;
    size_t common;              /* length of the common prefix of all matches */
    char list[1024];
};

static void note_match(struct match_ctx *m, const char *name, bool is_dir)
{
    if (strncmp(name, m->prefix, m->plen) != 0)
        return;
    if (m->count == 0) {
        strlcpy(m->first, name, sizeof m->first);
        m->common = strlen(name);
    } else {
        size_t i = 0;
        while (i < m->common && m->first[i] == name[i])
            i++;
        m->common = i;
    }
    m->count++;
    size_t l = strlen(m->list);
    if (l + strlen(name) + 3 < sizeof m->list) {
        strcat(m->list, name);
        if (is_dir)
            strcat(m->list, "/");
        strcat(m->list, "  ");
    }
}

static void cmd_collect(const struct shell_cmd *c, void *ctx)
{
    note_match(ctx, c->name, false);
}

/* Try to complete the word that ends at `pos`. Returns true if the line changed. */
static bool complete(char *line, int *pos, int *len)
{
    int ws = *pos;
    while (ws > 0 && line[ws - 1] != ' ')
        ws--;
    char word[LINE_MAX];
    memcpy(word, line + ws, (size_t)(*pos - ws));
    word[*pos - ws] = '\0';

    struct match_ctx m;
    memset(&m, 0, sizeof m);
    bool first_word = true;
    for (int i = 0; i < ws; i++)
        if (line[i] != ' ')
            first_word = false;

    char dirpath[VFS_PATH_MAX];
    const char *base = word;
    if (first_word && !strchr(word, '/')) {
        m.prefix = word;
        m.plen = strlen(word);
        shell_for_each(cmd_collect, &m);
    } else {
        char *slash = strrchr(word, '/');
        if (slash) {
            size_t dl = (size_t)(slash - word) + 1;
            memcpy(dirpath, word, dl);
            dirpath[dl] = '\0';
            base = slash + 1;
        } else {
            strcpy(dirpath, ".");
        }
        m.prefix = base;
        m.plen = strlen(base);
        struct dirent de;
        for (uint32_t i = 0; vfs_readdir(dirpath, i, &de) == 0; i++)
            note_match(&m, de.name, de.type == VFS_DIR);
    }
    if (m.count == 0)
        return false;

    if (m.count > 1 && m.common == m.plen) {
        kprintf("\n%s\n", m.list);               /* ambiguous: show choices */
        return true;                              /* caller redraws the prompt */
    }
    /* insert the missing characters (plus a space if unique and not a dir) */
    char add[VFS_NAME_MAX + 4];
    size_t n = m.common - m.plen;
    memcpy(add, m.first + m.plen, n);
    if (m.count == 1) {
        struct vfs_stat st;
        char full[VFS_PATH_MAX];
        if (first_word && !strchr(word, '/')) {
            add[n++] = ' ';
        } else {
            ksnprintf(full, sizeof full, "%s%s", strcmp(dirpath, ".") ? dirpath : "", m.first);
            if (vfs_stat(full, &st) == 0 && st.type == VFS_DIR)
                add[n++] = '/';
            else
                add[n++] = ' ';
        }
    }
    add[n] = '\0';
    if (n == 0 || *len + (int)n >= LINE_MAX - 1)
        return false;
    memmove(line + *pos + n, line + *pos, (size_t)(*len - *pos) + 1);
    memcpy(line + *pos, add, n);
    for (size_t i = 0; i < n; i++)
        console_putc(add[i]);
    *pos += (int)n;
    *len += (int)n;
    redraw_tail(line, *pos, *len, 0);
    return false;
}

static void erase_line(int pos, int len)
{
    for (int i = 0; i < pos; i++)
        console_putc('\b');
    for (int i = 0; i < len; i++)
        console_putc(' ');
    for (int i = 0; i < len; i++)
        console_putc('\b');
}

/* Read one line with editing. Returns false on Ctrl+C (line abandoned). */
static bool read_line(char *line)
{
    int len = 0, pos = 0;
    int hist_pos = hist_count;                   /* hist_count = "the new line" */
    char saved[LINE_MAX] = "";

    line[0] = '\0';
    for (;;) {
        console_flush();
        int k = console_getkey();

        switch (k) {
        case '\n':
            console_putc('\n');
            line[len] = '\0';
            return true;
        case 3:                                  /* Ctrl+C */
            kprintf("^C\n");
            line[0] = '\0';
            return false;
        case '\b': case 127:
            if (pos > 0) {
                memmove(line + pos - 1, line + pos, (size_t)(len - pos) + 1);
                pos--;
                len--;
                console_putc('\b');
                redraw_tail(line, pos, len, 1);
            }
            break;
        case KEY_DELETE: case 4:
            if (pos < len) {
                memmove(line + pos, line + pos + 1, (size_t)(len - pos));
                len--;
                redraw_tail(line, pos, len, 1);
            }
            break;
        case KEY_LEFT:
            if (pos > 0) { pos--; console_putc('\b'); }
            break;
        case KEY_RIGHT:
            if (pos < len) { console_putc(line[pos]); pos++; }
            break;
        case KEY_HOME: case 1:
            while (pos > 0) { pos--; console_putc('\b'); }
            break;
        case KEY_END: case 5:
            while (pos < len) { console_putc(line[pos]); pos++; }
            break;
        case 21:                                 /* Ctrl+U: clear the line */
            erase_line(pos, len);
            len = pos = 0;
            line[0] = '\0';
            break;
        case 11:                                 /* Ctrl+K: delete to end */
            if (pos < len) {
                int n = len - pos;
                line[pos] = '\0';
                len = pos;
                redraw_tail(line, pos, len, n);
            }
            break;
        case 23: {                               /* Ctrl+W: delete previous word */
            int p0 = pos;
            while (p0 > 0 && line[p0 - 1] == ' ') p0--;
            while (p0 > 0 && line[p0 - 1] != ' ') p0--;
            if (p0 < pos) {
                int n = pos - p0;
                memmove(line + p0, line + pos, (size_t)(len - pos) + 1);
                for (int i = 0; i < n; i++) console_putc('\b');
                len -= n;
                pos = p0;
                redraw_tail(line, pos, len, n);
            }
            break;
        }
        case 12:                                 /* Ctrl+L: clear screen */
            console_clear();
            show_prompt();
            for (int i = 0; i < len; i++) console_putc(line[i]);
            for (int i = len; i > pos; i--) console_putc('\b');
            break;
        case KEY_UP: case KEY_DOWN: {
            if (k == KEY_UP && hist_pos == 0)
                break;
            if (k == KEY_DOWN && hist_pos >= hist_count)
                break;
            if (hist_pos == hist_count)
                strlcpy(saved, line, sizeof saved);
            hist_pos += (k == KEY_UP) ? -1 : 1;
            erase_line(pos, len);
            const char *src = hist_pos == hist_count ? saved
                              : history[(hist_next - hist_count + hist_pos + HIST_MAX * 2) % HIST_MAX];
            strlcpy(line, src, LINE_MAX);
            len = pos = (int)strlen(line);
            for (int i = 0; i < len; i++) console_putc(line[i]);
            break;
        }
        case KEY_PGUP:
            console_scroll(10);
            break;
        case KEY_PGDN:
            console_scroll(-10);
            break;
        case '\t':
            if (complete(line, &pos, &len)) {
                show_prompt();
                for (int i = 0; i < len; i++) console_putc(line[i]);
                for (int i = len; i > pos; i--) console_putc('\b');
            }
            break;
        default:
            if (k >= 32 && k < 127 && len < LINE_MAX - 1) {
                memmove(line + pos + 1, line + pos, (size_t)(len - pos) + 1);
                line[pos] = (char)k;
                len++;
                console_putc((char)k);
                pos++;
                redraw_tail(line, pos, len, 0);
            }
            break;
        }
    }
}

static void remember(const char *line)
{
    if (hist_count > 0 &&
        strcmp(history[(hist_next + HIST_MAX - 1) % HIST_MAX], line) == 0)
        return;
    strlcpy(history[hist_next], line, LINE_MAX);
    hist_next = (hist_next + 1) % HIST_MAX;
    if (hist_count < HIST_MAX)
        hist_count++;
}

int shell_history_count(void) { return hist_count; }
const char *shell_history_get(int i)           /* 0 = oldest */
{
    return history[(hist_next - hist_count + i + HIST_MAX * 2) % HIST_MAX];
}
int shell_last_status(void) { return last_status; }

void shell_run(void)
{
    char line[LINE_MAX];

    shell_exit_requested = false;
    console_set_color(CON_CYAN);
    kprintf("%s %s", MYOS_NAME, MYOS_VERSION);
    console_set_color(CON_NORMAL);
    kprintf(" - type 'help' for commands, Tab completes, PgUp/PgDn scrolls.\n");

    while (!shell_exit_requested) {
        show_prompt();
        if (!read_line(line))
            continue;
        if (line[0] == '\0')
            continue;
        remember(line);
        shell_exec(line);
    }
}
