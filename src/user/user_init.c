/*
 * user_init.c - YOUR playground. user_init() runs once at boot, before the
 * shell starts. Add shell commands below with SHELL_COMMAND.
 * Try them in the shell: hello, fsdemo.
 */
#include "kernel.h"
#include "shell.h"
#include "vfs.h"
#include "kprintf.h"
#include "kerror.h"
#include "kstring.h"

static int cmd_hello(int argc, char **argv)
{
    kprintf("Hello, %s! This command lives in src/user/user_init.c\n",
            argc > 1 ? argv[1] : "student");
    return 0;
}
SHELL_COMMAND("hello", "hello [name]", "Example custom command", cmd_hello);

/* Shows the file API: make a folder, write a file, read it back. */
static int cmd_fsdemo(int argc, char **argv)
{
    char buf[64];
    (void)argc; (void)argv;

    vfs_mkdir("/tmp");
    int r = vfs_write_file("/tmp/demo.txt", "written from C code\n", 20);
    if (r < 0) {
        kprintf("write failed: %s\n", kstrerror(r));
        return 1;
    }
    ssize_t n = vfs_read_file("/tmp/demo.txt", buf, sizeof buf - 1);
    if (n < 0) {
        kprintf("read failed: %s\n", kstrerror((int)n));
        return 1;
    }
    buf[n] = '\0';
    kprintf("read back %d bytes: %s", (int)n, buf);
    kprintf("Tip: to use a disk, run  lsblk, mkfs myfs hda, mount hda /mnt myfs\n");
    return 0;
}
SHELL_COMMAND("fsdemo", "fsdemo", "Example: create, write, read a file", cmd_fsdemo);

void user_init(void)
{
    /* put code here that should run once at boot */
}
