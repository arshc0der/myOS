/* console.c - console front end; forwards to the active back end + serial. */
#include "console.h"
#include "serial.h"

static const struct console_backend *backend;
static void (*capture_fn)(char, void *);
static void *capture_ctx;

/* A copy of recent output so a new back end can show earlier messages. */
#define HIST_SIZE 32768
static char hist[HIST_SIZE];
static uint32_t hist_head;                      /* next write position   */
static bool hist_wrapped;

void console_set_backend(const struct console_backend *b)
{
    backend = b;
    if (b && b->clear)
        b->clear();
}

const struct console_backend *console_get_backend(void) { return backend; }

void console_init(void) { console_set_backend(&vga_console); }

void console_set_capture(void (*fn)(char, void *), void *ctx)
{
    capture_fn = fn;
    capture_ctx = ctx;
}

void console_get_capture(void (**fn)(char, void *), void **ctx)
{
    *fn = capture_fn;
    *ctx = capture_ctx;
}

void console_putc(char c)
{
    if (capture_fn) {
        capture_fn(c, capture_ctx);
        return;
    }
    serial_putc(c);
    hist[hist_head] = c;
    if (++hist_head == HIST_SIZE) {
        hist_head = 0;
        hist_wrapped = true;
    }
    if (backend)
        backend->putc(c);
}

void console_write(const char *s)
{
    while (*s)
        console_putc(*s++);
    console_flush();
}

void console_clear(void)
{
    if (backend && backend->clear)
        backend->clear();
}

void console_set_color(uint8_t a)
{
    if (backend && backend->set_color)
        backend->set_color(a);
}

void console_scroll(int lines)
{
    if (backend && backend->scroll)
        backend->scroll(lines);
}

void console_flush(void)
{
    if (backend && backend->flush)
        backend->flush();
}

void console_replay(void)
{
    if (!backend)
        return;
    uint32_t start = hist_wrapped ? hist_head : 0;
    uint32_t n = hist_wrapped ? HIST_SIZE : hist_head;

    for (uint32_t i = 0; i < n; i++)
        backend->putc(hist[(start + i) % HIST_SIZE]);
    console_flush();
}

/* Copy the most recent output (up to max-1 characters) into dst. */
size_t console_history_copy(char *dst, size_t max)
{
    uint32_t total = hist_wrapped ? HIST_SIZE : hist_head;
    uint32_t n = total < max - 1 ? total : (uint32_t)(max - 1);
    uint32_t start = (hist_head + HIST_SIZE - n) % HIST_SIZE;

    for (uint32_t i = 0; i < n; i++)
        dst[i] = hist[(start + i) % HIST_SIZE];
    dst[n] = '\0';
    return n;
}
