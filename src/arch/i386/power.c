/* power.c - reboot and power off, for real PCs and common emulators. */
#include "kernel.h"
#include "io.h"
#include "console.h"

NORETURN void system_halt(void)
{
    cpu_cli();
    for (;;)
        cpu_hlt();
}

NORETURN void system_reboot(void)
{
    cpu_cli();
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 2))
            break;
    outb(0x64, 0xFE);                   /* keyboard controller reset line */
    system_halt();
}

NORETURN void system_poweroff(void)
{
    cpu_cli();
    outw(0x604, 0x2000);                /* QEMU (q35 / newer)  */
    outw(0xB004, 0x2000);               /* QEMU (older), Bochs */
    outw(0x4004, 0x3400);               /* VirtualBox          */
    static const char s[] = "Shutdown"; /* Bochs               */
    for (const char *p = s; *p; p++)
        outb(0x8900, (uint8_t)*p);
    console_write("\nIt is now safe to turn off the computer.\n");
    system_halt();
}
