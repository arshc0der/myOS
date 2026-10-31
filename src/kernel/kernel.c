/*
 * kernel.c - kernel_main(): the C entry point, called from boot.S.
 *
 * Boot order (each step needs the ones before it):
 *   1. serial + boot info + console   so we can print
 *   2. GDT, IDT, PIC                  CPU tables and interrupt controller
 *   3. PMM, paging, heap              memory
 *   4. scheduler + timer              multitasking
 */
#include "gui.h"
#include "usb.h"
#include "kernel.h"
#include "multiboot.h"
#include "serial.h"
#include "console.h"
#include "kprintf.h"
#include "log.h"
#include "gdt.h"
#include "isr.h"
#include "pic.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "sched.h"
#include "timer.h"
#include "io.h"
#include "panic.h"
#include "input.h"
#include "vfs.h"
#include "gfx.h"
#include "fbcon.h"
#include "shell.h"
#include "blockdev.h"

static void print_banner(void)
{
    console_set_color(CON_CYAN);
    kprintf("%s %s", MYOS_NAME, MYOS_VERSION);
    console_set_color(CON_NORMAL);
    kprintf(" (%s)\n", MYOS_CODENAME);
}

/* The first real task: runs tests, then (later) the shell or the GUI. */
static void init_main(void *arg)
{
    (void)arg;

    if (boot_has_arg("selftest")) {
        selftest_run();
        if (boot_has_arg("fbdump") && gfx_available())
            gfx_dump_serial();
        if (boot_has_arg("poweroff"))
            system_poweroff();
    }
    user_init();
    if (boot_has_arg("gui") && gui_supported())
        gui_run();
    for (;;) {
        shell_run();
        kprintf("(the main shell cannot be closed; restarting it)\n");
    }
}

void kernel_main(uint32_t magic, uint32_t mbi_addr)
{
    serial_init();
    boot_info_init(magic, mbi_addr);
    console_init();
    print_banner();
    log_info("boot", "loader: %s, cmdline: \"%s\"",
             boot_info.loader, boot_info.cmdline);

    gdt_init();
    idt_init();
    pic_init();
    log_info("cpu", "GDT, IDT and PIC ready");

    pmm_init();
    log_info("pmm", "%u MiB usable RAM, %u pages free",
             (pmm_total_pages() * 4) / 1024, pmm_free_pages_count());
    vmm_init();
    log_info("vmm", "paging enabled (identity map, page 0 unmapped)");
    heap_init();
    if (gfx_init()) {                   /* graphical framebuffer (UEFI / VBE) */
        console_set_backend(&fbcon_console);
        console_replay();               /* show everything printed so far */
        log_info("console", "text console is drawn on the framebuffer (%dx%d)",
                 fbcon_cols(), fbcon_rows());
    }
    {
        struct heap_stats hs;
        heap_get_stats(&hs);
        log_info("heap", "%u KiB kernel heap", hs.total_bytes / 1024);
    }

    sched_init();
    timer_init();
    sched_start();
    cpu_sti();
    log_info("sched", "scheduler running (%s, %u Hz)",
             sched_get_policy()->name, TIMER_HZ);

    /* Everything else runs in a normal task, never in the idle task. */
    input_init();
    keyboard_init();
    if (mouse_init())
        log_info("input", "PS/2 mouse found");
    usb_start();                        /* USB keyboards and mice (xHCI) */
    vfs_init();                         /* root ramfs + /dev */
    ata_init();                         /* IDE disks (hda, hdb ...) */
    if (ramdisk_create("ram0", MiB(4)) == 0)
        log_info("block", "ram0 is a blank 4 MiB RAM disk for experiments");
    for (int i = 0; i < boot_info.module_count; i++) {      /* GRUB modules */
        char nm[16];
        ksnprintf(nm, sizeof nm, "mod%d", i);
        ramdisk_create_from_memory(nm, (void *)boot_info.modules[i].start,
                                   boot_info.modules[i].end - boot_info.modules[i].start);
    }

    if (task_create("init", init_main, NULL) < 0)
        PANIC("cannot create the init task");

    for (;;) {                          /* this is the idle task */
        sched_reap();
        console_flush();
        cpu_hlt_wait();
    }
}
