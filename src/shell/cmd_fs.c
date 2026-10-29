/* cmd_fs.c - file, disk and filesystem commands. */
#include "shell.h"
#include "vfs.h"
#include "blockdev.h"
#include "kprintf.h"
#include "kstring.h"
#include "kerror.h"
#include "console.h"
#include "heap.h"

static int fail(const char *what, int err)
{
    console_set_color(CON_RED);
    kprintf("%s: %s\n", what, kstrerror(err));
    console_set_color(CON_NORMAL);
    return 1;
}

static void print_entry(const struct dirent *de, bool longfmt)
{
    if (longfmt)
        kprintf("%c %8u  ", de->type == VFS_DIR ? 'd' : de->type == VFS_CHAR ? 'c' :
                             de->type == VFS_BLOCK ? 'b' : '-', de->size);
    if (de->type == VFS_DIR)
        console_set_color(CON_CYAN);
    else if (de->type == VFS_CHAR || de->type == VFS_BLOCK)
        console_set_color(CON_YELLOW);
    kprintf("%s", de->name);
    console_set_color(CON_NORMAL);
    kprintf(longfmt ? "\n" : "  ");
}

/* ---------- ls / cd / pwd ---------- */

static int cmd_ls(int argc, char **argv)
{
    bool longfmt = false;
    const char *path = ".";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0)
            longfmt = true;
        else
            path = argv[i];
    }
    struct vfs_stat st;
    int r = vfs_stat(path, &st);
    if (r < 0)
        return fail(path, r);
    if (st.type != VFS_DIR) {
        struct dirent de = { .ino = st.ino, .type = st.type, .size = st.size };
        strlcpy(de.name, path, sizeof de.name);
        print_entry(&de, longfmt);
        kprintf("\n");
        return 0;
    }
    struct dirent de;
    int n = 0;
    for (uint32_t i = 0; (r = vfs_readdir(path, i, &de)) == 0; i++) {
        print_entry(&de, longfmt);
        n++;
    }
    if (!longfmt && n)
        kprintf("\n");
    return r == -ENOENT ? 0 : fail(path, r);
}
SHELL_COMMAND("ls", "ls [-l] [path]", "List a directory", cmd_ls);

static int cmd_cd(int argc, char **argv)
{
    int r = vfs_chdir(argc > 1 ? argv[1] : "/");
    return r < 0 ? fail(argc > 1 ? argv[1] : "/", r) : 0;
}
SHELL_COMMAND("cd", "cd [path]", "Change directory", cmd_cd);

static int cmd_pwd(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("%s\n", vfs_getcwd());
    return 0;
}
SHELL_COMMAND("pwd", "pwd", "Print the current directory", cmd_pwd);

/* ---------- cat / head / wc / hexdump ---------- */

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: cat <file>...\n");
        return 1;
    }
    for (int a = 1; a < argc; a++) {
        int fd = vfs_open(argv[a], O_RDONLY);
        if (fd < 0)
            return fail(argv[a], fd);
        char buf[256];
        ssize_t n;
        while ((n = vfs_read(fd, buf, sizeof buf)) > 0)
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                console_putc((c == '\n' || c == '\t' || (c >= 32 && c < 127)) ? c : '.');
            }
        vfs_close(fd);
        if (n < 0)
            return fail(argv[a], (int)n);
    }
    return 0;
}
SHELL_COMMAND("cat", "cat <file>...", "Print files", cmd_cat);

static int cmd_head(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: head <file> [lines]\n");
        return 1;
    }
    int lines = argc > 2 ? katoi(argv[2]) : 10;
    int fd = vfs_open(argv[1], O_RDONLY);
    if (fd < 0)
        return fail(argv[1], fd);
    char c;
    while (lines > 0 && vfs_read(fd, &c, 1) == 1) {
        console_putc(c);
        if (c == '\n')
            lines--;
    }
    vfs_close(fd);
    return 0;
}
SHELL_COMMAND("head", "head <file> [lines]", "Print the first lines of a file", cmd_head);

static int cmd_wc(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: wc <file>\n");
        return 1;
    }
    int fd = vfs_open(argv[1], O_RDONLY);
    if (fd < 0)
        return fail(argv[1], fd);
    uint32_t lines = 0, words = 0, bytes = 0;
    bool inword = false;
    char buf[256];
    ssize_t n;
    while ((n = vfs_read(fd, buf, sizeof buf)) > 0)
        for (ssize_t i = 0; i < n; i++) {
            bytes++;
            if (buf[i] == '\n')
                lines++;
            bool sp = isspace(buf[i]);
            if (!sp && !inword)
                words++;
            inword = !sp;
        }
    vfs_close(fd);
    kprintf("%u lines, %u words, %u bytes  %s\n", lines, words, bytes, argv[1]);
    return 0;
}
SHELL_COMMAND("wc", "wc <file>", "Count lines, words and bytes", cmd_wc);

static int cmd_hexdump(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: hexdump <file> [offset] [length]\n");
        return 1;
    }
    uint32_t off = argc > 2 ? kstrtoul(argv[2], NULL, 0) : 0;
    uint32_t len = argc > 3 ? kstrtoul(argv[3], NULL, 0) : 256;
    int fd = vfs_open(argv[1], O_RDONLY);
    if (fd < 0)
        return fail(argv[1], fd);
    vfs_seek(fd, (int32_t)off, SEEK_SET);
    uint8_t row[16];
    uint32_t done = 0;
    while (done < len) {
        ssize_t n = vfs_read(fd, row, MIN(16u, len - done));
        if (n <= 0)
            break;
        kprintf("%08x  ", off + done);
        for (int i = 0; i < 16; i++) {
            if (i < n)
                kprintf("%02x ", row[i]);
            else
                kprintf("   ");
            if (i == 7)
                kprintf(" ");
        }
        kprintf(" |");
        for (int i = 0; i < n; i++)
            console_putc(row[i] >= 32 && row[i] < 127 ? (char)row[i] : '.');
        kprintf("|\n");
        done += (uint32_t)n;
    }
    vfs_close(fd);
    return 0;
}
SHELL_COMMAND("hexdump", "hexdump <file> [offset] [len]", "Show bytes in hex (works on /dev/ram0 too)", cmd_hexdump);

/* ---------- mkdir / touch / rm / cp / mv / stat ---------- */

static int cmd_mkdir(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) {
        kprintf("usage: mkdir <dir>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        int r = vfs_mkdir(argv[i]);
        if (r < 0)
            rc = fail(argv[i], r);
    }
    return rc;
}
SHELL_COMMAND("mkdir", "mkdir <dir>...", "Create directories", cmd_mkdir);

static int cmd_touch(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) {
        kprintf("usage: touch <file>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        struct vfs_stat st;
        if (vfs_stat(argv[i], &st) == 0)
            continue;
        int r = vfs_create(argv[i]);
        if (r < 0)
            rc = fail(argv[i], r);
    }
    return rc;
}
SHELL_COMMAND("touch", "touch <file>...", "Create empty files", cmd_touch);

static int rm_recursive(const char *path, int depth)
{
    struct vfs_stat st;
    int r = vfs_stat(path, &st);

    if (r < 0)
        return r;
    if (st.type == VFS_DIR && depth < 12) {
        struct dirent de;
        while (vfs_readdir(path, 0, &de) == 0) {
            char child[VFS_PATH_MAX];
            ksnprintf(child, sizeof child, "%s/%s", path, de.name);
            if ((r = rm_recursive(child, depth + 1)) < 0)
                return r;
        }
    }
    return vfs_unlink(path);
}

static int cmd_rm(int argc, char **argv)
{
    bool recursive = false;
    int rc = 0, count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            recursive = true;
            continue;
        }
        count++;
        int r = recursive ? rm_recursive(argv[i], 0) : vfs_unlink(argv[i]);
        if (r < 0)
            rc = fail(argv[i], r);
    }
    if (!count) {
        kprintf("usage: rm [-r] <path>...\n");
        return 1;
    }
    return rc;
}
SHELL_COMMAND("rm", "rm [-r] <path>...", "Delete files or (empty / with -r) directories", cmd_rm);

static int copy_file(const char *src, const char *dst)
{
    int in = vfs_open(src, O_RDONLY);
    if (in < 0)
        return in;
    int out = vfs_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        vfs_close(in);
        return out;
    }
    char *buf = kmalloc(4096);
    int r = buf ? 0 : -ENOMEM;
    ssize_t n;
    while (r == 0 && (n = vfs_read(in, buf, 4096)) > 0) {
        ssize_t done = 0;
        while (done < n) {
            ssize_t w = vfs_write(out, buf + done, (size_t)(n - done));
            if (w <= 0) {
                r = w < 0 ? (int)w : -EIO;
                break;
            }
            done += w;
        }
    }
    if (r == 0 && n < 0)
        r = (int)n;
    kfree(buf);
    vfs_close(in);
    vfs_close(out);
    return r;
}

static int cmd_cp(int argc, char **argv)
{
    if (argc != 3) {
        kprintf("usage: cp <from> <to>\n");
        return 1;
    }
    struct vfs_stat st;
    char dst[VFS_PATH_MAX];
    strlcpy(dst, argv[2], sizeof dst);
    if (vfs_stat(argv[2], &st) == 0 && st.type == VFS_DIR) {    /* copy into a directory */
        const char *base = strrchr(argv[1], '/');
        ksnprintf(dst, sizeof dst, "%s/%s", argv[2], base ? base + 1 : argv[1]);
    }
    int r = copy_file(argv[1], dst);
    return r < 0 ? fail("cp", r) : 0;
}
SHELL_COMMAND("cp", "cp <from> <to>", "Copy a file", cmd_cp);

static int cmd_mv(int argc, char **argv)
{
    if (argc != 3) {
        kprintf("usage: mv <from> <to>\n");
        return 1;
    }
    struct vfs_stat st;
    char dst[VFS_PATH_MAX];
    strlcpy(dst, argv[2], sizeof dst);
    if (vfs_stat(argv[2], &st) == 0 && st.type == VFS_DIR) {
        const char *base = strrchr(argv[1], '/');
        ksnprintf(dst, sizeof dst, "%s/%s", argv[2], base ? base + 1 : argv[1]);
    }
    /* The VFS has no rename yet, so mv = copy + delete. (A nice exercise:
     * add a `rename` operation to struct node_ops!) */
    int r = copy_file(argv[1], dst);
    if (r == 0)
        r = vfs_unlink(argv[1]);
    return r < 0 ? fail("mv", r) : 0;
}
SHELL_COMMAND("mv", "mv <from> <to>", "Move/rename a file (copy + delete)", cmd_mv);

static int cmd_stat(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: stat <path>\n");
        return 1;
    }
    struct vfs_stat st;
    int r = vfs_stat(argv[1], &st);
    if (r < 0)
        return fail(argv[1], r);
    static const char *tn[] = { "?", "file", "directory", "char device", "block device" };
    kprintf("  path : %s\n  type : %s\n  size : %u bytes\n  inode: %u\n", argv[1],
            st.type < 5 ? tn[st.type] : "?", st.size, st.ino);
    return 0;
}
SHELL_COMMAND("stat", "stat <path>", "Show information about a file", cmd_stat);

static void tree_walk(const char *path, int depth, int *count)
{
    struct dirent de;

    for (uint32_t i = 0; vfs_readdir(path, i, &de) == 0; i++) {
        for (int d = 0; d < depth; d++)
            kprintf("  ");
        print_entry(&de, false);
        kprintf("\n");
        (*count)++;
        if (de.type == VFS_DIR && depth < 6) {
            char child[VFS_PATH_MAX];
            ksnprintf(child, sizeof child, "%s/%s", strcmp(path, "/") ? path : "", de.name);
            tree_walk(child, depth + 1, count);
        }
    }
}

static int cmd_tree(int argc, char **argv)
{
    const char *p = argc > 1 ? argv[1] : ".";
    char abs[VFS_PATH_MAX];
    int count = 0;

    if (vfs_normalize(p, abs, sizeof abs) < 0)
        return 1;
    kprintf("%s\n", abs);
    tree_walk(abs, 1, &count);
    kprintf("%d entries\n", count);
    return 0;
}
SHELL_COMMAND("tree", "tree [path]", "Show a directory tree", cmd_tree);

/* ---------- devices, mounts, filesystems ---------- */

static void print_dev(struct blockdev *d, void *ctx)
{
    (void)ctx;
    uint32_t kib = d->sector_count / 2;
    kprintf("  %-8s %-10s %7u sectors  ", d->name, d->kind, d->sector_count);
    if (kib >= 1024)
        kprintf("%u MiB", kib / 1024);
    else
        kprintf("%u KiB", kib);
    if (d->model[0])
        kprintf("  %s", d->model);
    kprintf("\n");
}

static int cmd_lsblk(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("  NAME     KIND          SIZE\n");
    blockdev_for_each(print_dev, NULL);
    return 0;
}
SHELL_COMMAND("lsblk", "lsblk", "List disks, RAM disks and partitions", cmd_lsblk);

static void print_mount(const struct vfs_mount *m, void *ctx)
{
    (void)ctx;
    kprintf("  %-8s on %-12s type %s\n", m->dev ? m->dev->name : "none", m->path,
            m->driver->name);
}

static void print_driver(const struct fs_driver *d, void *ctx)
{
    (void)ctx;
    kprintf(" %s%s", d->name, d->mkfs ? "(mkfs)" : "");
}

static int cmd_mount(int argc, char **argv)
{
    if (argc == 1) {
        kprintf("Mounted filesystems:\n");
        vfs_for_each_mount(print_mount, NULL);
        kprintf("Filesystem types:");
        vfs_for_each_driver(print_driver, NULL);
        kprintf("\n");
        return 0;
    }
    if (argc != 4) {
        kprintf("usage: mount <device|none> <path> <fstype>\n"
                "  example: mount ram0 /mnt myfs\n");
        return 1;
    }
    int r = vfs_mount(argv[1], argv[2], argv[3]);
    return r < 0 ? fail("mount", r) : 0;
}
SHELL_COMMAND("mount", "mount [<dev> <path> <fstype>]", "Mount a filesystem (no arguments: list)", cmd_mount);

static int cmd_umount(int argc, char **argv)
{
    if (argc != 2) {
        kprintf("usage: umount <path>\n");
        return 1;
    }
    int r = vfs_umount(argv[1]);
    return r < 0 ? fail("umount", r) : 0;
}
SHELL_COMMAND("umount", "umount <path>", "Unmount a filesystem", cmd_umount);

static int cmd_mkfs(int argc, char **argv)
{
    if (argc != 3) {
        kprintf("usage: mkfs <fstype> <device>\n  example: mkfs myfs ram0\n");
        return 1;
    }
    int r = vfs_mkfs(argv[1], argv[2]);
    if (r < 0)
        return fail("mkfs", r);
    kprintf("Created a %s filesystem on %s\n", argv[1], argv[2]);
    return 0;
}
SHELL_COMMAND("mkfs", "mkfs <fstype> <device>", "Format a device (ERASES it)", cmd_mkfs);

static int cmd_df(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("Mounted filesystems:\n");
    vfs_for_each_mount(print_mount, NULL);
    return 0;
}
SHELL_COMMAND("df", "df", "Show mounted filesystems", cmd_df);

static int cmd_ramdisk(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "create") == 0) {
        uint32_t kib = kstrtoul(argv[3], NULL, 0);
        int r = ramdisk_create(argv[2], kib * 1024);
        return r < 0 ? fail("ramdisk", r) : 0;
    }
    if (argc == 3 && strcmp(argv[1], "destroy") == 0) {
        int r = ramdisk_destroy(argv[2]);
        return r < 0 ? fail("ramdisk", r) : 0;
    }
    kprintf("usage: ramdisk create <name> <size-in-KiB>\n       ramdisk destroy <name>\n");
    return 1;
}
SHELL_COMMAND("ramdisk", "ramdisk create <name> <KiB> | destroy <name>", "Make or remove a RAM disk", cmd_ramdisk);

static int cmd_mkpart(int argc, char **argv)
{
    if (argc != 4) {
        kprintf("usage: mkpart <disk> <start-sector> <sectors>\n"
                "  Writes a partition table with one partition, then registers it.\n");
        return 1;
    }
    struct blockdev *d = blockdev_find(argv[1]);
    if (!d)
        return fail(argv[1], -ENODEV);
    int r = partition_write_mbr(d, kstrtoul(argv[2], NULL, 0), kstrtoul(argv[3], NULL, 0), 0x83);
    if (r < 0)
        return fail("mkpart", r);
    int n = partition_scan(d);
    kprintf("%d partition(s) registered; see lsblk\n", n);
    return 0;
}
SHELL_COMMAND("mkpart", "mkpart <disk> <start> <count>", "Create a one-partition MBR", cmd_mkpart);
