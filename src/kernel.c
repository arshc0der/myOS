/*
 * kernel.c - the C entry point of MyOS.
 *
 * boot.S calls kernel_main() after GRUB hands control to the kernel.
 * GRUB passes two values, which boot.S forwards to us:
 *   magic - 0x2BADB002 if we were really loaded by a Multiboot bootloader
 *   addr  - address of the Multiboot info structure (memory size, etc.)
 */

#include <stdint.h>

extern void console_clear(void);
extern void console_write(const char *);
extern void shell_run(void);
extern void multiboot_init(uint32_t magic, uint32_t addr);

void kernel_main(uint32_t magic, uint32_t addr)
{
    /* Save the boot information so commands like meminfo can use it. */
    multiboot_init(magic, addr);

    console_clear();
    console_write("Starting MyOS kernel...\n");

    shell_run();

    /* shell_run() never returns. If it ever does, stop the CPU. */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}