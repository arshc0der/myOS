
#include <stddef.h>

extern void console_write(const char *);
extern void console_putc(char);
extern void console_clear(void);

__attribute__((noreturn))
void kernel_panic(const char *message)
{
    __asm__ volatile ("cli");

    console_write("\n");
    console_write("================================\n");
    console_write("          MYOS KERNEL PANIC\n");
    console_write("================================\n");

    console_write("A fatal kernel error occurred.\n");
    console_write("Reason: ");

    if (message != NULL) {
        console_write(message);
    } else {
        console_write("Unknown error");
    }

    console_putc('\n');
    console_write("System halted.\n");

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
