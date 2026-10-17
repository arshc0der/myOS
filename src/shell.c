
#include <stddef.h>

#define INPUT_SIZE 128

extern void console_write(const char *);
extern void console_putc(char);
extern void console_clear(void);
extern char keyboard_getchar(void);

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

static void help(void)
{
    console_write("Commands:\n");
    console_write("  help   - show commands\n");
    console_write("  about  - about MyOS\n");
    console_write("  clear  - clear screen\n");
    console_write("  echo   - print text\n");
    console_write("  halt   - halt CPU\n");
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
        console_write("MyOS - My own operating system!\n");
        console_write("Shell written in C.\n");
    } else if (equal(s, "clear")) {
        console_clear();
    } else if (equal(s, "echo")) {
        console_write(args);
        console_putc('\n');
    } else if (equal(s, "halt")) {
        console_write("CPU halted.\n");

        __asm__ volatile ("cli");

        for (;;)
            __asm__ volatile ("hlt");
    } else {
        console_write("Unknown command: ");
        console_write(s);
        console_putc('\n');
    }
}

void shell_run(void)
{
    console_write("Welcome to MyOS!\n");
    console_write("Type help to begin.\n\n");

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
                    console_write("\b");
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
