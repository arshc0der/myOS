
#include <stddef.h>
#include <stdint.h>

#define INPUT_SIZE 128

extern void console_write(const char *);
extern void console_putc(char);
extern void console_clear(void);
extern char keyboard_getchar(void);

/*
 * Optional functions:
 * Implement these in other kernel files before enabling them.
 */
extern uint32_t boot_memory_kb(void);
extern int boot_memory_available(void);

static char input[INPUT_SIZE];

static int equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;

        a++;
        b++;
    }

    return *a == *b;
}

static char *skip_spaces(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;

    return s;
}

static void print_number(uint32_t value)
{
    char buffer[11];
    size_t n = 0;

    if (value == 0) {
        console_putc('0');
        return;
    }

    while (value > 0 && n < sizeof(buffer)) {
        buffer[n++] = (char)('0' + value % 10);
        value /= 10;
    }

    while (n > 0)
        console_putc(buffer[--n]);
}

static void print_line(const char *text)
{
    console_write(text);
    console_putc('\n');
}

static void help(void)
{
    console_write("MyOS shell commands\n");
    console_write("-------------------\n");
    console_write("help       - list commands\n");
    console_write("about      - about MyOS\n");
    console_write("version    - kernel version\n");
    console_write("clear      - clear screen\n");
    console_write("echo TEXT  - print text\n");
    console_write("meminfo    - boot memory information\n");
    console_write("cpuinfo    - architecture information\n");
    console_write("date       - show unavailable status\n");
    console_write("whoami     - show current environment\n");
    console_write("history    - show current session status\n");
    console_write("halt       - halt the CPU\n");
    console_write("shutdown   - halt the CPU\n");
    console_write("reboot     - reboot (not implemented)\n");
}

static void show_memory(void)
{
    console_write("Memory information\n");
    console_write("------------------\n");
/*
    if (boot_memory_available()) {
        console_write("Bootloader-reported memory: ");
        print_number(boot_memory_kb());
        console_write(" KiB\n");
    } else {
        console_write("Memory map not available yet.\n");
        console_write("Implement boot memory-map parsing first.\n");
    }
*/
    console_write("Kernel heap: not implemented\n");
    console_write("Physical page allocator: not implemented\n");
    console_write("Virtual memory manager: not implemented\n");
}

static void show_cpu(void)
{
    console_write("CPU / kernel information\n");
    console_write("------------------------\n");
    console_write("Architecture: x86\n");
    console_write("Kernel mode: 32-bit protected-mode target\n");
    console_write("Compiler: GCC-compatible C\n");
    console_write("Interrupt-driven scheduler: not implemented\n");
}

static void show_about(void)
{
    console_write("MyOS - My own operating system!\n");
    console_write("A hobby operating system written in C and x86 assembly.\n");
    console_write("Kernel version: 0.1\n");
}

static void show_status(void)
{
    console_write("MyOS session status\n");
    console_write("-------------------\n");
    console_write("Shell: running\n");
    console_write("Command history: not implemented\n");
    console_write("Filesystem: not implemented\n");
    console_write("User processes: not implemented\n");
}

static void run_command(char *s)
{
    s = skip_spaces(s);

    if (*s == '\0')
        return;

    char *args = s;

    while (*args && *args != ' ' && *args != '\t')
        args++;

    if (*args) {
        *args++ = '\0';
        args = skip_spaces(args);
    }

    if (equal(s, "help")) {
        help();

    } else if (equal(s, "about")) {
        show_about();

    } else if (equal(s, "version")) {
        console_write("MyOS version 0.1\n");

    } else if (equal(s, "clear")) {
        console_clear();

    } else if (equal(s, "echo")) {
        console_write(args);
        console_putc('\n');

    } else if (equal(s, "meminfo")) {
        show_memory();

    } else if (equal(s, "cpuinfo")) {
        show_cpu();

    } else if (equal(s, "whoami")) {
        console_write("kernel-shell\n");

    } else if (equal(s, "history")) {
        show_status();

    } else if (equal(s, "date")) {
        console_write("System clock/RTC support not implemented.\n");

    } else if (equal(s, "halt") || equal(s, "shutdown")) {
        console_write("MyOS halted. You can close the emulator.\n");

        __asm__ volatile ("cli");

        for (;;) {
            __asm__ volatile ("hlt");
        }

    } else if (equal(s, "reboot")) {
        console_write("Reboot is not implemented yet.\n");
        console_write("Add a keyboard-controller or ACPI reset driver.\n");

    } else {
        console_write("Unknown command: ");
        console_write(s);
        console_putc('\n');
        console_write("Type 'help' to see available commands.\n");
    }
}

void shell_run(void)
{
    console_write("\n");
    console_write("========================\n");
    console_write("       Welcome to MyOS\n");
    console_write("========================\n");
    console_write("Type 'help' to list commands.\n\n");

    for (;;) {
        console_write("MyOS> ");

        size_t n = 0;

        for (;;) {
            char c = keyboard_getchar();

            if (c == '\n' || c == '\r') {
                console_putc('\n');
                break;
            }

            if (c == '\b') {
                if (n > 0) {
                    n--;
                    console_write("\b \b");
                }
                continue;
            }

            if (c >= 32 && c <= 126 &&
                n < INPUT_SIZE - 1) {
                input[n++] = c;
                console_putc(c);
            }
        }

        input[n] = '\0';
        run_command(input);
    }
}
