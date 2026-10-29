/* kernel.h - top-level kernel functions. */
#ifndef KERNEL_H
#define KERNEL_H
#include "types.h"
#include "version.h"

void kernel_main(uint32_t magic, uint32_t mbi_addr);

/* power control (src/arch/i386/power.c) */
NORETURN void system_reboot(void);
NORETURN void system_poweroff(void);
NORETURN void system_halt(void);

/* self tests (src/test/selftest.c); returns number of failures */
int selftest_run(void);
/* Used by test code in other modules: TEST_CHECK(x == 3, "x is three"); */
void selftest_check(bool ok, const char *name, const char *file, int line);
#define TEST_CHECK(cond, name) selftest_check((cond), (name), __FILE__, __LINE__)

/* Print the display as an image over the serial port (testing aid). */
void gfx_dump_serial(void);

/* The code you write goes here (src/user/user_init.c). */
void user_init(void);

#endif
