/*
 * kprintf.h - formatted output.
 *
 * Supported: %d %i %u %x %X %o %c %s %p %% with flags '-', '0', '+', ' ',
 * a width, a .precision (for strings), and the length modifiers l and ll
 * (so 64-bit numbers work: %llu, %llx).
 */
#ifndef KPRINTF_H
#define KPRINTF_H

#include "types.h"

/* Print to the screen (and the serial port). */
int kprintf(const char *fmt, ...) PRINTF_LIKE(1, 2);
int kvprintf(const char *fmt, va_list ap);
void kputs(const char *s);

/* Format into a buffer (always NUL-terminated). Returns the length the
 * full output would have had, like snprintf. */
int ksnprintf(char *buf, size_t size, const char *fmt, ...) PRINTF_LIKE(3, 4);
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif
