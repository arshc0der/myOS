/* panic.c - fatal errors: show what happened, then stop. */
#include "panic.h"
#include "isr.h"
#include "console.h"
#include "kprintf.h"
#include "io.h"

void print_backtrace(uint32_t ebp)
{
    kprintf("Call trace:\n");
    for (int i = 0; i < 12 && ebp >= 0x100000 && (ebp & 3) == 0; i++) {
        uint32_t *frame = (uint32_t *)ebp;
        uint32_t ret = frame[1];
        if (ret == 0)
            break;
        kprintf("  [%d] 0x%08x\n", i, ret);
        if (frame[0] <= ebp)
            break;
        ebp = frame[0];
    }
}

static NORETURN void halt_forever(void)
{
    cpu_cli();
    for (;;)
        cpu_hlt();
}

static void banner(void)
{
    cpu_cli();
    console_set_color(0x4F);                /* white on red */
    kprintf("\n*** KERNEL PANIC ***");
    console_set_color(CON_RED);
    kprintf("\n");
}

NORETURN void kernel_panic(const char *message)
{
    banner();
    kprintf("%s\n", message);
    uint32_t ebp;
    __asm__ volatile ("movl %%ebp, %0" : "=r"(ebp));
    print_backtrace(ebp);
    kprintf("System halted.\n");
    halt_forever();
}

NORETURN void kernel_panic_fmt(const char *fmt, ...)
{
    char buf[160];
    va_list ap;

    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kernel_panic(buf);
}

NORETURN void kernel_panic_regs(const char *message, const struct regs *r)
{
    banner();
    kprintf("%s\n", message);
    kprintf("EIP=%08x  CS=%04x  EFLAGS=%08x  vector=%u err=%x\n",
            r->eip, r->cs, r->eflags, r->int_no, r->err_code);
    kprintf("EAX=%08x EBX=%08x ECX=%08x EDX=%08x\n",
            r->eax, r->ebx, r->ecx, r->edx);
    kprintf("ESI=%08x EDI=%08x EBP=%08x\n", r->esi, r->edi, r->ebp);
    print_backtrace(r->ebp);
    kprintf("System halted.\n");
    halt_forever();
}
