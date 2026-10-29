/* selftest_shell.c - tests that drive the shell like a user would. */
#include "kernel.h"
#include "shell.h"
#include "console.h"
#include "kstring.h"
#include "vfs.h"

#define T TEST_CHECK

struct cap { char *buf; size_t n, max; };

static void cap_char(char c, void *ctx)
{
    struct cap *c2 = ctx;

    if (c2->n + 1 < c2->max)
        c2->buf[c2->n++] = c;
}

/* Run a command line and collect what it printed. Returns the exit status. */
static int run(const char *line, char *out, size_t max)
{
    struct cap c = { out, 0, max };
    void (*old_fn)(char, void *);
    void *old_ctx;

    console_get_capture(&old_fn, &old_ctx);
    console_set_capture(cap_char, &c);
    int rc = shell_exec(line);
    console_set_capture(old_fn, old_ctx);
    out[c.n] = '\0';
    return rc;
}

void selftest_shell(void)
{
    static char out[2048];

    vfs_chdir("/");
    T(run("echo hello world", out, sizeof out) == 0 && strcmp(out, "hello world\n") == 0,
      "shell: echo");
    run("echo \"a  b\" 'c d'", out, sizeof out);
    T(strcmp(out, "a  b c d\n") == 0, "shell: quoting keeps spaces");
    T(run("nosuchcommand", out, sizeof out) == 127, "shell: unknown command gives 127");
    run("echo one; echo two", out, sizeof out);
    T(strcmp(out, "one\ntwo\n") == 0, "shell: ';' runs several commands");

    run("echo hi > /tmp/s1.txt; cat /tmp/s1.txt", out, sizeof out);
    T(strcmp(out, "hi\n") == 0, "shell: '>' redirects into a file");
    run("echo more >> /tmp/s1.txt; cat /tmp/s1.txt", out, sizeof out);
    T(strcmp(out, "hi\nmore\n") == 0, "shell: '>>' appends");
    run("wc /tmp/s1.txt", out, sizeof out);
    T(strstr(out, "2 lines") != NULL, "shell: wc");

    run("mkdir /tmp/d1; cp /tmp/s1.txt /tmp/d1; ls /tmp/d1", out, sizeof out);
    T(strstr(out, "s1.txt") != NULL, "shell: mkdir, cp into a directory, ls");
    run("mv /tmp/d1/s1.txt /tmp/d1/moved.txt; ls /tmp/d1", out, sizeof out);
    T(strstr(out, "moved.txt") && !strstr(out, "s1.txt"), "shell: mv");
    run("cd /tmp; pwd", out, sizeof out);
    T(strcmp(out, "/tmp\n") == 0, "shell: cd and pwd");
    run("cd /; rm -r /tmp/d1; ls /tmp", out, sizeof out);
    T(!strstr(out, "d1"), "shell: rm -r");
    run("rm /tmp/s1.txt", out, sizeof out);

    run("cd /; ls /dev", out, sizeof out);
    T(strstr(out, "null") && strstr(out, "ram0"), "shell: ls /dev");
    run("hexdump /dev/zero 0 16", out, sizeof out);
    T(strstr(out, "00 00 00 00") != NULL, "shell: hexdump");

    run("mkfs myfs ram0", out, sizeof out);
    T(strstr(out, "Created") != NULL, "shell: mkfs myfs ram0");
    run("mount ram0 /mnt myfs; echo on-disk > /mnt/note; cat /mnt/note; umount /mnt", out, sizeof out);
    T(strcmp(out, "on-disk\n") == 0, "shell: mount, write, read, umount");
    run("mount ram0 /mnt myfs; mount", out, sizeof out);
    T(strstr(out, "ram0") && strstr(out, "myfs"), "shell: mount lists mounts");
    run("umount /mnt", out, sizeof out);
    run("lsblk", out, sizeof out);
    T(strstr(out, "ram0") != NULL, "shell: lsblk");

    run("ps", out, sizeof out);
    T(strstr(out, "idle") && strstr(out, "init"), "shell: ps shows idle and init");
    run("help", out, sizeof out);
    T(strstr(out, "mkfs") && strstr(out, "spawn"), "shell: help lists commands");
    run("meminfo", out, sizeof out);
    T(strstr(out, "Kernel heap") != NULL, "shell: meminfo");
    run("cpuinfo", out, sizeof out);
    T(strstr(out, "Vendor") != NULL, "shell: cpuinfo");
    run("lspci", out, sizeof out);
    T(strstr(out, "device(s)") != NULL, "shell: lspci");
    run("date", out, sizeof out);
    T(strstr(out, "20") != NULL, "shell: date");
}
