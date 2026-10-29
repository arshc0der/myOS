/* kprintf.c - formatted output (see include/kprintf.h). */
#include "kprintf.h"
#include "kstring.h"
#include "console.h"
#include "serial.h"

/* Output sink: either a buffer (snprintf) or the console. */
struct out {
    char  *buf;
    size_t size;
    size_t pos;         /* chars that would have been written */
    bool   to_console;
};

static void out_char(struct out *o, char c)
{
    if (o->to_console)
        console_putc(c);
    else if (o->pos + 1 < o->size)
        o->buf[o->pos] = c;
    o->pos++;
}

static void out_pad(struct out *o, char c, int n)
{
    while (n-- > 0)
        out_char(o, c);
}

static int format_number(char *tmp, uint64_t v, unsigned base, bool upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;

    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    return n;
}

static void do_format(struct out *o, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_char(o, *fmt);
            continue;
        }
        fmt++;

        bool left = false, zero = false, plus = false, space = false;
        for (;; fmt++) {
            if (*fmt == '-')      left = true;
            else if (*fmt == '0') zero = true;
            else if (*fmt == '+') plus = true;
            else if (*fmt == ' ') space = true;
            else break;
        }

        int width = 0, prec = -1;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = true; width = -width; }
            fmt++;
        } else {
            while (isdigit(*fmt))
                width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (isdigit(*fmt))
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }

        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z')    { fmt++; }

        char tmp[40];
        const char *s = NULL;
        int len = 0;
        char sign = 0;
        bool numeric = false;
        char c = *fmt;

        switch (c) {
        case 'd': case 'i': {
            int64_t v = lng >= 2 ? va_arg(ap, int64_t)
                                 : (int64_t)va_arg(ap, int);
            uint64_t u = v < 0 ? -(uint64_t)v : (uint64_t)v;
            if (v < 0)      sign = '-';
            else if (plus)  sign = '+';
            else if (space) sign = ' ';
            len = format_number(tmp, u, 10, false);
            numeric = true;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = lng >= 2 ? va_arg(ap, uint64_t)
                                  : (uint64_t)va_arg(ap, unsigned);
            unsigned base = c == 'u' ? 10 : c == 'o' ? 8 : 16;
            len = format_number(tmp, v, base, c == 'X');
            numeric = true;
            break;
        }
        case 'p': {
            uint32_t v = (uint32_t)va_arg(ap, void *);
            len = format_number(tmp, v, 16, false);
            while (len < 8)
                tmp[len++] = '0';
            tmp[len++] = 'x';
            tmp[len++] = '0';
            numeric = true;
            break;
        }
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            s = NULL;
            len = 1;
            numeric = true;     /* tmp holds chars in forward order */
            out_pad(o, ' ', left ? 0 : width - 1);
            out_char(o, tmp[0]);
            out_pad(o, ' ', left ? width - 1 : 0);
            continue;
        case 's':
            s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            len = prec >= 0 ? (int)strnlen(s, (size_t)prec) : (int)strlen(s);
            break;
        case '%':
            out_char(o, '%');
            continue;
        case '\0':
            return;
        default:
            out_char(o, '%');
            out_char(o, c);
            continue;
        }

        if (s) {                                    /* string */
            out_pad(o, ' ', left ? 0 : width - len);
            for (int i = 0; i < len; i++)
                out_char(o, s[i]);
            out_pad(o, ' ', left ? width - len : 0);
        } else if (numeric) {                       /* number (digits reversed) */
            int total = len + (sign ? 1 : 0);
            if (zero && !left) {
                if (sign) out_char(o, sign);
                out_pad(o, '0', width - total);
            } else {
                out_pad(o, ' ', left ? 0 : width - total);
                if (sign) out_char(o, sign);
            }
            for (int i = len - 1; i >= 0; i--)
                out_char(o, tmp[i]);
            out_pad(o, ' ', left ? width - total : 0);
        }
    }
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0, false };

    do_format(&o, fmt, ap);
    if (size)
        buf[o.pos < size ? o.pos : size - 1] = '\0';
    return (int)o.pos;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int kvprintf(const char *fmt, va_list ap)
{
    struct out o = { NULL, 0, 0, true };

    do_format(&o, fmt, ap);
    console_flush();
    return (int)o.pos;
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}

void kputs(const char *s)
{
    console_write(s);
}
