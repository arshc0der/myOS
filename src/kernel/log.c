/* log.c - kernel log. Everything goes to serial; INFO and up to the screen. */
#include "log.h"
#include "kprintf.h"
#include "serial.h"
#include "console.h"
#include "timer.h"

static int console_level = LOG_INFO;

void log_set_console_level(int level) { console_level = level; }

void klog(int level, const char *tag, const char *fmt, ...)
{
    char line[200];
    va_list ap;
    uint32_t ms = timer_ms();
    int n = ksnprintf(line, sizeof line, "[%3u.%03u] %s: ",
                      ms / 1000, ms % 1000, tag);

    va_start(ap, fmt);
    kvsnprintf(line + n, sizeof line - (size_t)n - 2, fmt, ap);
    va_end(ap);

    size_t len = 0;
    while (line[len])
        len++;
    line[len++] = '\n';
    line[len] = '\0';

    if (level >= console_level) {
        uint8_t color = level >= LOG_ERROR ? CON_RED :
                        level == LOG_WARN  ? CON_YELLOW : CON_NORMAL;
        console_set_color(color);
        console_write(line);
        console_set_color(CON_NORMAL);
    } else {
        serial_write(line);
    }
}
