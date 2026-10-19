/*
 * shell.c - the MyOS command-line shell.
 *
 * Features:
 *   - commands: help, about, version, sysinfo, meminfo, cpuinfo, lspci,
 *     date, history, echo, clear, reboot, halt, shutdown
 *   - Up/Down arrows recall previous commands (command history)
 *   - Page Up / Page Down scroll the screen, Home / End jump to the
 *     oldest / newest output
 */

#include <stddef.h>
#include <stdint.h>
#include "keys.h"

#define MYOS_VERSION  "1.2.0"
#define INPUT_SIZE    128
#define HISTORY_SIZE  16
#define PAGE_LINES    20

/* VGA colors (low nibble = text, high nibble = background). */
#define C_NORMAL  0x07      /* light grey  */
#define C_TITLE   0x0B      /* light cyan  */
#define C_LABEL   0x0E      /* yellow      */
#define C_PROMPT  0x0A      /* light green */
#define C_ERROR   0x0C      /* light red   */
#define C_DIM     0x08      /* dark grey   */

/* console.c */
extern void console_write(const char *);
extern void console_putc(char);
extern void console_clear(void);
extern void console_scroll(int lines);
extern void console_set_color(uint8_t color);
extern void console_put_dec(uint32_t value);
extern void console_put_hex(uint32_t value, int digits);
extern void console_put_hex64(uint32_t hi, uint32_t lo);

/* keyboard.c */
extern int keyboard_getchar(void);

/* multiboot.c */
extern int boot_memory_available(void);
extern uint32_t mb_mem_lower_kb(void);
extern uint32_t mb_mem_upper_kb(void);
extern const char *mb_bootloader_name(void);
extern uint32_t mb_mmap_count(void);
extern int mb_mmap_get(uint32_t index, uint64_t *base,
                       uint64_t *length, uint32_t *type);
extern const char *mb_type_name(uint32_t type);

/* cpu.c */
extern void cpu_vendor(char out[13]);
extern int cpu_brand(char out[49]);
extern void cpu_signature(uint32_t *family, uint32_t *model,
                          uint32_t *stepping);
extern uint32_t cpu_features_edx(void);
extern uint32_t cpu_features_ecx(void);
extern int cpu_hypervisor(char out[13]);

/* pci.c */
extern uint32_t pci_count(void);
extern void pci_list(void);

/* rtc.c */
extern void rtc_read(uint32_t t[6]);

/* Symbols created by linker.ld: where the kernel sits in memory. */
extern char _kernel_start[];
extern char _kernel_end[];

static char input[INPUT_SIZE];
static char history[HISTORY_SIZE][INPUT_SIZE];
static size_t history_count;

/* ------------------------------------------------------------------ */
/* Small helpers (there is no standard library, so we write our own)   */
/* ------------------------------------------------------------------ */

static size_t str_len(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;

    return n;
}

static void str_copy(char *dst, const char *src)
{
    while ((*dst++ = *src++))
        ;
}

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

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void wait_keyboard_ready(void)
{
    /* Wait until the keyboard controller can accept a command. */
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++)
        ;
}

/* ------------------------------------------------------------------ */
/* Pretty printing                                                     */
/* ------------------------------------------------------------------ */

/* Title followed by an underline. */
static void heading(const char *title)
{
    console_set_color(C_TITLE);
    console_write(title);
    console_putc('\n');

    for (size_t i = 0; i < str_len(title); i++)
        console_putc('-');

    console_putc('\n');
    console_set_color(C_NORMAL);
}

/* Print `s` and pad with spaces up to `width` columns. */
static void print_pad(const char *s, size_t width)
{
    size_t len = str_len(s);

    console_write(s);

    while (len++ < width)
        console_putc(' ');
}

/* Right-align a number in `width` columns. */
static void print_dec_right(uint32_t value, size_t width)
{
    size_t digits = 1;

    for (uint32_t v = value; v >= 10; v /= 10)
        digits++;

    while (digits++ < width)
        console_putc(' ');

    console_put_dec(value);
}

/* "Label           : " in color, then back to normal text. */
static void field(const char *name)
{
    console_set_color(C_LABEL);
    print_pad(name, 18);
    console_write(": ");
    console_set_color(C_NORMAL);
}

/* 130048 KiB (127 MiB), or just 639 KiB for small sizes. */
static void print_kib(uint32_t kib)
{
    console_put_dec(kib);
    console_write(" KiB");

    if (kib >= 1024) {
        console_write(" (");
        console_put_dec(kib >> 10);
        console_write(" MiB)");
    }
}

static void print_2digits(uint32_t v)
{
    if (v < 10)
        console_putc('0');

    console_put_dec(v);
}

static void print_error(const char *msg)
{
    console_set_color(C_ERROR);
    console_write(msg);
    console_set_color(C_NORMAL);
}

/* ------------------------------------------------------------------ */
/* Information commands                                                */
/* ------------------------------------------------------------------ */

/* Total size of usable RAM in KiB, from the bootloader's memory map. */
static uint32_t usable_ram_kib(void)
{
    uint64_t total = 0;
    uint64_t base, length;
    uint32_t type;

    for (uint32_t i = 0; mb_mmap_get(i, &base, &length, &type); i++) {
        if (type == 1)
            total += length >> 10;
    }

    if (total > 0xFFFFFFFFull)
        total = 0xFFFFFFFFull;

    return (uint32_t)total;
}

static void show_memory(void)
{
    uint32_t kernel_start = (uint32_t)(uintptr_t)_kernel_start;
    uint32_t kernel_end   = (uint32_t)(uintptr_t)_kernel_end;

    heading("Memory information");

    if (!boot_memory_available()) {
        print_error("No memory information from the bootloader.\n");
        console_write("Boot MyOS through GRUB (Multiboot) to see it.\n");
        return;
    }

    uint32_t lower = mb_mem_lower_kb();
    uint32_t upper = mb_mem_upper_kb();

    field("Lower memory");
    print_kib(lower);
    console_putc('\n');

    field("Upper memory");
    print_kib(upper);
    console_putc('\n');

    field("Total (basic)");
    print_kib(lower + upper);
    console_putc('\n');

    field("Kernel image");
    console_put_hex(kernel_start, 8);
    console_write(" - ");
    console_put_hex(kernel_end, 8);
    console_write(" (");
    console_put_dec((kernel_end - kernel_start) >> 10);
    console_write(" KiB)\n");

    uint32_t count = mb_mmap_count();

    field("Memory map");

    if (count == 0) {
        console_write("not provided\n");
    } else {
        console_put_dec(count);
        console_write(" region(s)\n\n");

        console_set_color(C_DIM);
        console_write("   #  ");
        print_pad("Base", 18);
        console_write("  ");
        print_pad("Size (KiB)", 10);
        console_write("  Type\n");
        console_set_color(C_NORMAL);

        uint64_t base, length;
        uint32_t type;

        for (uint32_t i = 0; mb_mmap_get(i, &base, &length, &type); i++) {
            uint64_t kib = length >> 10;

            if (kib > 0xFFFFFFFFull)
                kib = 0xFFFFFFFFull;

            console_write("  ");
            print_dec_right(i, 2);
            console_write("  ");
            console_put_hex64((uint32_t)(base >> 32), (uint32_t)base);
            console_write("  ");
            print_dec_right((uint32_t)kib, 10);
            console_write("  ");
            console_write(mb_type_name(type));
            console_putc('\n');
        }

        console_putc('\n');
        field("Usable RAM");
        print_kib(usable_ram_kib());
        console_putc('\n');
    }

    console_set_color(C_DIM);
    console_write("Heap and page allocator: not implemented yet\n");
    console_set_color(C_NORMAL);
}

struct feature {
    uint8_t bit;
    const char *name;
};

static const struct feature features_edx[] = {
    { 0, "FPU" }, { 4, "TSC" }, { 5, "MSR" }, { 6, "PAE" },
    { 9, "APIC" }, { 15, "CMOV" }, { 23, "MMX" }, { 24, "FXSR" },
    { 25, "SSE" }, { 26, "SSE2" },
};

static const struct feature features_ecx[] = {
    { 0, "SSE3" }, { 9, "SSSE3" }, { 19, "SSE4.1" }, { 20, "SSE4.2" },
    { 23, "POPCNT" }, { 28, "AVX" },
};

static void print_features(uint32_t reg, const struct feature *list,
                           size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (reg & (1u << list[i].bit)) {
            console_write(list[i].name);
            console_putc(' ');
        }
    }

    console_putc('\n');
}

static void show_cpu(void)
{
    char vendor[13];
    char brand[49];
    char hv[13];
    uint32_t family, model, stepping;

    heading("CPU information");

    cpu_vendor(vendor);
    field("Vendor");
    console_write(vendor);
    console_putc('\n');

    if (cpu_brand(brand)) {
        const char *name = brand;

        while (*name == ' ')
            name++;

        field("Model");
        console_write(name);
        console_putc('\n');
    }

    cpu_signature(&family, &model, &stepping);
    field("Family/Model/Step");
    console_put_dec(family);
    console_write(" / ");
    console_put_dec(model);
    console_write(" / ");
    console_put_dec(stepping);
    console_putc('\n');

    field("Features");
    print_features(cpu_features_edx(), features_edx,
                   sizeof(features_edx) / sizeof(features_edx[0]));

    field("");
    print_features(cpu_features_ecx(), features_ecx,
                   sizeof(features_ecx) / sizeof(features_ecx[0]));

    field("Virtualization");
    if (cpu_hypervisor(hv)) {
        console_write("yes (");
        console_write(hv);
        console_write(")\n");
    } else {
        console_write("not detected (real hardware)\n");
    }

    field("Architecture");
    console_write("x86, 32-bit protected mode\n");

    console_set_color(C_DIM);
    console_write("Scheduler: not implemented yet\n");
    console_set_color(C_NORMAL);
}

static void show_sysinfo(void)
{
    char vendor[13];
    char brand[49];
    char hv[13];
    const char *loader = mb_bootloader_name();

    heading("System summary");

    field("OS");
    console_write("MyOS " MYOS_VERSION " (32-bit x86)\n");

    field("CPU");
    if (cpu_brand(brand)) {
        const char *name = brand;

        while (*name == ' ')
            name++;

        console_write(name);
    } else {
        cpu_vendor(vendor);
        console_write(vendor);
    }
    console_putc('\n');

    field("Memory");
    if (boot_memory_available()) {
        uint32_t usable = usable_ram_kib();

        if (usable == 0)
            usable = mb_mem_lower_kb() + mb_mem_upper_kb();

        print_kib(usable);
        console_write(" usable\n");
    } else {
        console_write("unknown\n");
    }

    field("Bootloader");
    console_write(loader ? loader : "unknown");
    console_putc('\n');

    field("PCI devices");
    console_put_dec(pci_count());
    console_putc('\n');

    field("Virtualization");
    if (cpu_hypervisor(hv)) {
        console_write("yes (");
        console_write(hv);
        console_write(")\n");
    } else {
        console_write("not detected\n");
    }
}

static void show_pci(void)
{
    heading("PCI devices");
    pci_list();
}

static void show_date(void)
{
    uint32_t t[6];

    rtc_read(t);

    field("Date");
    console_put_dec(t[0]);
    console_putc('-');
    print_2digits(t[1]);
    console_putc('-');
    print_2digits(t[2]);
    console_putc(' ');
    print_2digits(t[3]);
    console_putc(':');
    print_2digits(t[4]);
    console_putc(':');
    print_2digits(t[5]);
    console_write("  (RTC clock, usually UTC)\n");
}

/* ------------------------------------------------------------------ */
/* Other commands                                                      */
/* ------------------------------------------------------------------ */

static void help(void)
{
    heading("MyOS shell commands");

    console_write("help       - list commands\n");
    console_write("about      - about MyOS\n");
    console_write("version    - kernel version\n");
    console_write("sysinfo    - system summary\n");
    console_write("meminfo    - memory size and memory map\n");
    console_write("cpuinfo    - CPU vendor, model and features\n");
    console_write("lspci      - list PCI devices\n");
    console_write("date       - date and time from the RTC clock\n");
    console_write("history    - commands typed so far\n");
    console_write("echo TEXT  - print text\n");
    console_write("clear      - clear the screen and scrollback\n");
    console_write("reboot     - restart the computer\n");
    console_write("halt       - halt the CPU (also: shutdown)\n");

    console_putc('\n');
    console_set_color(C_DIM);
    console_write("Keys: Up/Down = command history\n");
    console_write("      PgUp/PgDn = scroll, Home/End = oldest/newest\n");
    console_set_color(C_NORMAL);
}

static void show_about(void)
{
    console_write("MyOS - My own operating system!\n");
    console_write("A hobby operating system written in C and x86 assembly.\n");
    console_write("Kernel version: " MYOS_VERSION "\n");
}

static void show_history(void)
{
    if (history_count == 0) {
        console_write("No commands yet.\n");
        return;
    }

    for (size_t i = 0; i < history_count; i++) {
        console_set_color(C_DIM);
        console_write("  ");
        print_dec_right((uint32_t)(i + 1), 2);
        console_write("  ");
        console_set_color(C_NORMAL);
        console_write(history[i]);
        console_putc('\n');
    }
}

static void halt_cpu(void)
{
    console_write("MyOS halted. You can close the emulator.\n");

    __asm__ volatile ("cli");

    for (;;)
        __asm__ volatile ("hlt");
}

static void reboot(void)
{
    console_write("Rebooting...\n");

    /* Ask the keyboard controller to pulse the CPU reset line. */
    wait_keyboard_ready();
    outb(0x64, 0xFE);

    /* If that did not work, stop here. */
    print_error("Reboot failed. The CPU is halted.\n");

    __asm__ volatile ("cli");

    for (;;)
        __asm__ volatile ("hlt");
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
        console_write("MyOS version " MYOS_VERSION "\n");
    } else if (equal(s, "sysinfo")) {
        show_sysinfo();
    } else if (equal(s, "meminfo")) {
        show_memory();
    } else if (equal(s, "cpuinfo")) {
        show_cpu();
    } else if (equal(s, "lspci")) {
        show_pci();
    } else if (equal(s, "date")) {
        show_date();
    } else if (equal(s, "history")) {
        show_history();
    } else if (equal(s, "echo")) {
        console_write(args);
        console_putc('\n');
    } else if (equal(s, "clear")) {
        console_clear();
    } else if (equal(s, "reboot")) {
        reboot();
    } else if (equal(s, "halt") || equal(s, "shutdown")) {
        halt_cpu();
    } else {
        print_error("Unknown command: ");
        print_error(s);
        console_putc('\n');
        console_write("Type 'help' to see available commands.\n");
    }
}

/* ------------------------------------------------------------------ */
/* Command history and line editing                                    */
/* ------------------------------------------------------------------ */

static void history_add(const char *line)
{
    if (*line == '\0')
        return;

    /* Do not store the same command twice in a row. */
    if (history_count > 0 && equal(history[history_count - 1], line))
        return;

    if (history_count == HISTORY_SIZE) {
        for (size_t i = 1; i < HISTORY_SIZE; i++)
            str_copy(history[i - 1], history[i]);

        history_count--;
    }

    str_copy(history[history_count++], line);
}

/* Erase what is typed on screen and replace it with `text`. */
static size_t replace_line(size_t typed, const char *text)
{
    while (typed > 0) {
        console_write("\b \b");
        typed--;
    }

    size_t n = str_len(text);

    if (n > INPUT_SIZE - 1)
        n = INPUT_SIZE - 1;

    for (size_t i = 0; i < n; i++)
        input[i] = text[i];

    input[n] = '\0';
    console_write(input);

    return n;
}

void shell_run(void)
{
    console_set_color(C_TITLE);
    console_write("\n");
    console_write("========================================\n");
    console_write("             Welcome to MyOS\n");
    console_write("========================================\n");
    console_set_color(C_NORMAL);
    console_write("Type 'help' to list commands.\n");
    console_set_color(C_DIM);
    console_write("PgUp/PgDn scroll the screen, Up/Down recall commands.\n\n");
    console_set_color(C_NORMAL);

    for (;;) {
        console_set_color(C_PROMPT);
        console_write("MyOS> ");
        console_set_color(C_NORMAL);

        size_t n = 0;
        size_t hist_pos = history_count;    /* == count means "new line" */

        for (;;) {
            int c = keyboard_getchar();

            if (c == '\n' || c == '\r') {
                console_putc('\n');
                break;
            }

            if (c == '\b') {
                if (n > 0) {
                    n--;
                    console_write("\b \b");
                }
            } else if (c == KEY_PGUP) {
                console_scroll(PAGE_LINES);
            } else if (c == KEY_PGDN) {
                console_scroll(-PAGE_LINES);
            } else if (c == KEY_HOME) {
                console_scroll(100000);
            } else if (c == KEY_END) {
                console_scroll(-100000);
            } else if (c == KEY_UP) {
                if (hist_pos > 0) {
                    hist_pos--;
                    n = replace_line(n, history[hist_pos]);
                }
            } else if (c == KEY_DOWN) {
                if (hist_pos < history_count) {
                    hist_pos++;
                    n = replace_line(n, hist_pos == history_count
                                        ? "" : history[hist_pos]);
                }
            } else if (c >= 32 && c <= 126 && n < INPUT_SIZE - 1) {
                input[n++] = (char)c;
                console_putc((char)c);
            }
        }

        input[n] = '\0';
        history_add(input);
        run_command(input);
    }
}
