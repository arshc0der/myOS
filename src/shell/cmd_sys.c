/* cmd_sys.c - system information and general commands. */
#include "shell.h"
#include "kernel.h"
#include "kprintf.h"
#include "kstring.h"
#include "kerror.h"
#include "console.h"
#include "multiboot.h"
#include "pmm.h"
#include "heap.h"
#include "sched.h"
#include "timer.h"
#include "hw.h"
#include "gfx.h"
#include "blockdev.h"
#include "input.h"
#include "vmm.h"

extern int shell_history_count(void);
extern const char *shell_history_get(int i);
extern bool shell_exit_requested;

/* ---------- help ---------- */

#define MAX_CMDS 160
static const struct shell_cmd *list[MAX_CMDS];
static int nlist;

static void collect(const struct shell_cmd *c, void *ctx)
{
    (void)ctx;
    if (nlist < MAX_CMDS)
        list[nlist++] = c;
}

static int cmd_help(int argc, char **argv)
{
    nlist = 0;
    shell_for_each(collect, NULL);
    for (int i = 1; i < nlist; i++) {                   /* insertion sort */
        const struct shell_cmd *k = list[i];
        int j = i - 1;
        while (j >= 0 && strcmp(list[j]->name, k->name) > 0) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = k;
    }
    if (argc > 1) {
        const struct shell_cmd *c = shell_find(argv[1]);
        if (!c) {
            kprintf("help: no command called '%s'\n", argv[1]);
            return 1;
        }
        kprintf("%s\n  usage: %s\n", c->help, c->usage);
        return 0;
    }
    console_set_color(CON_BRIGHT);
    kprintf("Commands (type 'help <name>' for details):\n");
    console_set_color(CON_NORMAL);
    for (int i = 0; i < nlist; i++) {
        console_set_color(CON_CYAN);
        kprintf("  %-10s", list[i]->name);
        console_set_color(CON_NORMAL);
        kprintf(" %s\n", list[i]->help);
    }
    return 0;
}
SHELL_COMMAND("help", "help [command]", "List commands, or explain one", cmd_help);

/* ---------- about / version ---------- */

static int cmd_about(int argc, char **argv)
{
    (void)argc; (void)argv;
    console_set_color(CON_CYAN);
    kprintf("%s %s \"%s\"\n", MYOS_NAME, MYOS_VERSION, MYOS_CODENAME);
    console_set_color(CON_NORMAL);
    kprintf("A small 32-bit x86 operating system written from scratch in C and\n"
            "assembly. It has a memory manager, a preemptive scheduler, a virtual\n"
            "file system with several filesystems, an ATA disk driver and a GUI.\n"
            "Source and documentation: https://github.com/arshc0der/myOS\n");
    return 0;
}
SHELL_COMMAND("about", "about", "About MyOS", cmd_about);

static int cmd_version(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("%s %s (%s)\n", MYOS_NAME, MYOS_VERSION, MYOS_CODENAME);
    return 0;
}
SHELL_COMMAND("version", "version", "Print the version number", cmd_version);

static int cmd_uname(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("%s %s i386\n", MYOS_NAME, MYOS_VERSION);
    return 0;
}
SHELL_COMMAND("uname", "uname", "Print system name and architecture", cmd_uname);

/* ---------- memory ---------- */

static void print_kib(uint64_t kib)
{
    kprintf("%llu KiB", kib);
    if (kib >= 1024)
        kprintf(" (%llu MiB)", kib / 1024);
}

static int cmd_meminfo(int argc, char **argv)
{
    (void)argc; (void)argv;
    console_set_color(CON_BRIGHT);
    kprintf("Firmware memory map:\n");
    console_set_color(CON_NORMAL);
    uint64_t usable = 0;
    for (int i = 0; i < boot_info.mmap_count; i++) {
        const struct boot_mmap_entry *e = &boot_info.mmap[i];
        kprintf("  0x%08llx - 0x%08llx  %-16s  %llu KiB\n", e->base,
                e->base + e->length - 1, boot_mem_type_name(e->type), e->length / 1024);
        if (e->type == MB_MEM_AVAILABLE)
            usable += e->length;
    }
    if (boot_info.mmap_count == 0)
        kprintf("  (none reported)  lower %u KiB, upper %u KiB\n",
                boot_info.mem_lower_kb, boot_info.mem_upper_kb);
    kprintf("  usable by firmware: "); print_kib(usable / 1024); kprintf("\n");

    console_set_color(CON_BRIGHT);
    kprintf("Physical pages (4 KiB each):\n");
    console_set_color(CON_NORMAL);
    kprintf("  total %u, used %u, free %u  ->  free ", pmm_total_pages(),
            pmm_used_pages(), pmm_free_pages_count());
    print_kib((uint64_t)pmm_free_pages_count() * 4);
    kprintf("\n");

    struct heap_stats h;
    heap_get_stats(&h);
    console_set_color(CON_BRIGHT);
    kprintf("Kernel heap:\n");
    console_set_color(CON_NORMAL);
    kprintf("  size %u KiB, used %u KiB in %u allocations, free %u KiB, largest free block %u KiB\n",
            h.total_bytes / 1024, h.used_bytes / 1024, h.alloc_count,
            h.free_bytes / 1024, h.largest_free / 1024);
    return 0;
}
SHELL_COMMAND("meminfo", "meminfo", "Memory map, page and heap usage", cmd_meminfo);

static int cmd_heap(int argc, char **argv)
{
    (void)argc; (void)argv;
    int bad = heap_check();
    kprintf("heap check: %s\n", bad ? "PROBLEMS FOUND" : "ok");
    return bad ? 1 : 0;
}
SHELL_COMMAND("heapcheck", "heapcheck", "Verify the kernel heap structure", cmd_heap);

/* ---------- cpu ---------- */

static int cmd_cpuinfo(int argc, char **argv)
{
    (void)argc; (void)argv;
    char vendor[13], brand[49], hv[13];
    uint32_t fam, model, step;

    cpu_vendor(vendor);
    cpu_signature(&fam, &model, &step);
    kprintf("Vendor:     %s\n", vendor);
    if (cpu_brand(brand)) {
        const char *b = brand;
        while (*b == ' ')
            b++;
        kprintf("Model name: %s\n", b);
    }
    kprintf("Family %u, model %u, stepping %u\n", fam, model, step);
    uint32_t edx = cpu_features_edx(), ecx = cpu_features_ecx();
    static const struct { uint32_t bit; const char *name; } fe[] = {
        {0, "fpu"}, {3, "pse"}, {4, "tsc"}, {5, "msr"}, {6, "pae"}, {9, "apic"},
        {15, "cmov"}, {23, "mmx"}, {24, "fxsr"}, {25, "sse"}, {26, "sse2"},
    };
    static const struct { uint32_t bit; const char *name; } fc[] = {
        {0, "sse3"}, {9, "ssse3"}, {19, "sse4.1"}, {20, "sse4.2"}, {23, "popcnt"},
        {25, "aes"}, {28, "avx"},
    };
    kprintf("Features:  ");
    for (unsigned i = 0; i < ARRAY_SIZE(fe); i++)
        if (edx & BIT(fe[i].bit))
            kprintf(" %s", fe[i].name);
    for (unsigned i = 0; i < ARRAY_SIZE(fc); i++)
        if (ecx & BIT(fc[i].bit))
            kprintf(" %s", fc[i].name);
    kprintf("\n");
    if (cpu_hypervisor(hv))
        kprintf("Running inside a virtual machine (hypervisor: %s)\n", hv);
    else
        kprintf("Running on real hardware (no hypervisor detected)\n");
    return 0;
}
SHELL_COMMAND("cpuinfo", "cpuinfo", "CPU vendor, model and features", cmd_cpuinfo);

/* ---------- pci ---------- */

static int cmd_lspci(int argc, char **argv)
{
    (void)argc; (void)argv;
    static struct pci_device devs[64];
    int n = pci_scan(devs, 64);

    for (int i = 0; i < n && i < 64; i++) {
        console_set_color(CON_CYAN);
        kprintf("%02x:%02x.%u", devs[i].bus, devs[i].slot, devs[i].func);
        console_set_color(CON_NORMAL);
        kprintf("  %04x:%04x  %s - %s\n", devs[i].vendor, devs[i].device,
                pci_class_name(devs[i].cls, devs[i].sub), pci_vendor_name(devs[i].vendor));
    }
    kprintf("%d device(s)\n", n);
    return 0;
}
SHELL_COMMAND("lspci", "lspci", "List PCI devices", cmd_lspci);

/* ---------- time ---------- */

static int cmd_date(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint32_t t[6];

    rtc_read(t);
    kprintf("%04u-%02u-%02u %02u:%02u:%02u (UTC)\n", t[0], t[1], t[2], t[3], t[4], t[5]);
    return 0;
}
SHELL_COMMAND("date", "date", "Show the date and time from the clock chip", cmd_date);

static int cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint32_t s = timer_uptime_sec();

    kprintf("up %u:%02u:%02u (%u ticks, %u task switches)\n", s / 3600, (s / 60) % 60,
            s % 60, timer_ticks(), sched_total_switches());
    return 0;
}
SHELL_COMMAND("uptime", "uptime", "Time since boot", cmd_uptime);

static int cmd_sleep(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: sleep <seconds>\n");
        return 1;
    }
    timer_sleep_ms((uint32_t)katoi(argv[1]) * 1000);
    return 0;
}
SHELL_COMMAND("sleep", "sleep <seconds>", "Wait (other tasks keep running)", cmd_sleep);

/* ---------- sysinfo ---------- */

static void count_disk(struct blockdev *d, void *ctx)
{
    (void)d;
    (*(int *)ctx)++;
}

static int cmd_sysinfo(int argc, char **argv)
{
    (void)argc; (void)argv;
    char vendor[13], brand[49];
    uint32_t s = timer_uptime_sec();
    int disks = 0;

    cpu_vendor(vendor);
    blockdev_for_each(count_disk, &disks);
    console_set_color(CON_CYAN);
    kprintf("%s %s", MYOS_NAME, MYOS_VERSION);
    console_set_color(CON_NORMAL);
    kprintf("\n  Boot loader : %s%s\n", boot_info.loader,
            boot_info.cmdline[0] ? "  (options: " : "");
    if (boot_info.cmdline[0])
        kprintf("                %s)\n", boot_info.cmdline);
    kprintf("  CPU         : %s", vendor);
    if (cpu_brand(brand)) {
        const char *b = brand;
        while (*b == ' ') b++;
        kprintf(" - %s", b);
    }
    kprintf("\n  Memory      : %u MiB usable, %u MiB free\n",
            pmm_total_pages() / 256, pmm_free_pages_count() / 256);
    kprintf("  Display     : %s", console_get_backend() ? console_get_backend()->name : "none");
    if (gfx_available())
        kprintf(" %dx%d", gfx_width(), gfx_height());
    kprintf("\n  Block devs  : %d\n", disks);
    kprintf("  Scheduler   : %s, %u tasks\n", sched_get_policy()->name, sched_task_count());
    kprintf("  Mouse       : %s\n", mouse_present() ? "PS/2 mouse" : "not found");
    kprintf("  Uptime      : %u:%02u:%02u\n", s / 3600, (s / 60) % 60, s % 60);
    return 0;
}
SHELL_COMMAND("sysinfo", "sysinfo", "Summary of the system", cmd_sysinfo);

/* ---------- misc ---------- */

static int cmd_echo(int argc, char **argv)
{
    bool newline = true;
    int i = 1;

    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = false;
        i = 2;
    }
    for (; i < argc; i++)
        kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
    if (newline)
        kprintf("\n");
    return 0;
}
SHELL_COMMAND("echo", "echo [-n] text...  (use > file to save)", "Print text", cmd_echo);

static int cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    console_clear();
    return 0;
}
SHELL_COMMAND("clear", "clear", "Clear the screen", cmd_clear);

static int cmd_history(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int i = 0; i < shell_history_count(); i++)
        kprintf("%3d  %s\n", i + 1, shell_history_get(i));
    return 0;
}
SHELL_COMMAND("history", "history", "Show recent commands", cmd_history);

static int cmd_dmesg(int argc, char **argv)
{
    (void)argc; (void)argv;
    static char buf[6000];

    console_history_copy(buf, sizeof buf);
    /* skip the (probably cut) first line */
    char *nl = strchr(buf, '\n');
    kprintf("%s", nl ? nl + 1 : buf);
    return 0;
}
SHELL_COMMAND("dmesg", "dmesg", "Show recent kernel messages", cmd_dmesg);

static int cmd_selftest(int argc, char **argv)
{
    (void)argc; (void)argv;
    return selftest_run() ? 1 : 0;
}
SHELL_COMMAND("selftest", "selftest", "Run the built-in kernel tests", cmd_selftest);

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("Rebooting...\n");
    system_reboot();
}
SHELL_COMMAND("reboot", "reboot", "Restart the computer", cmd_reboot);

static int cmd_halt(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("System halted. You can turn the power off.\n");
    system_halt();
}
SHELL_COMMAND("halt", "halt", "Stop the CPU", cmd_halt);

static int cmd_shutdown(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("Powering off...\n");
    system_poweroff();
}
SHELL_COMMAND("shutdown", "shutdown", "Power off (works in VMs)", cmd_shutdown);

static int cmd_exit(int argc, char **argv)
{
    (void)argc; (void)argv;
    shell_exit_requested = true;
    return 0;
}
SHELL_COMMAND("exit", "exit", "Leave the shell (closes a Terminal window)", cmd_exit);
