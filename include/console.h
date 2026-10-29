/*
 * console.h - text output.
 *
 * The console is split into a front end (this API, used by kprintf) and a
 * back end that actually draws characters. Today the back end is the VGA
 * text screen. Later a framebuffer text console (fbcon) can replace it
 * without changing any code that calls console_putc().
 */
#ifndef CONSOLE_H
#define CONSOLE_H
#include "types.h"

struct console_backend {
    const char *name;
    void (*putc)(char c);
    void (*clear)(void);
    void (*set_color)(uint8_t attr);
    void (*scroll)(int lines);          /* >0 older output, <0 newer */
    void (*flush)(void);                /* make pending output visible (may be NULL) */
};

void console_init(void);
void console_set_backend(const struct console_backend *b);
const struct console_backend *console_get_backend(void);

void console_putc(char c);
void console_write(const char *s);
void console_clear(void);
void console_set_color(uint8_t attr);   /* VGA colour attribute */
void console_scroll(int lines);
void console_flush(void);
/* Send everything printed to `fn` instead of the screen (used for `cmd > file`).
 * Pass NULL to switch back. */
void console_set_capture(void (*fn)(char c, void *ctx), void *ctx);
void console_get_capture(void (**fn)(char c, void *ctx), void **ctx);               /* show everything printed so far */
/* Everything printed so far (kept so a new back end can replay it). */
void console_replay(void);
size_t console_history_copy(char *dst, size_t max);

/* VGA colours for console_set_color(). */
#define CON_NORMAL  0x07
#define CON_BRIGHT  0x0F
#define CON_GREEN   0x0A
#define CON_CYAN    0x0B
#define CON_RED     0x0C
#define CON_YELLOW  0x0E

/* The built-in VGA text back end. */
extern const struct console_backend vga_console;

#endif
