/*
 * panic.h - fatal errors and assertions.
 */
#ifndef PANIC_H
#define PANIC_H

#include "types.h"

struct regs;    /* defined in isr.h */

/* Print a panic screen and stop the machine. */
NORETURN void kernel_panic(const char *message);
NORETURN void kernel_panic_fmt(const char *fmt, ...) PRINTF_LIKE(1, 2);
/* Same, but also shows the CPU registers captured by an exception. */
NORETURN void kernel_panic_regs(const char *message, const struct regs *r);

/* Print the call chain (return addresses) of the current code. */
void print_backtrace(uint32_t ebp);

#define PANIC(...) kernel_panic_fmt(__VA_ARGS__)

#define ASSERT(cond) \
    do { \
        if (!(cond)) \
            kernel_panic_fmt("assertion failed: %s (%s:%d)", \
                             #cond, __FILE__, __LINE__); \
    } while (0)

#endif
