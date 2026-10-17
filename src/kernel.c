
extern void console_clear(void);
extern void console_write(const char *);
extern void shell_run(void);

void kernel_main(void)
{
    console_clear();
    console_write("Starting MyOS kernel...\n");

    shell_run();

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
