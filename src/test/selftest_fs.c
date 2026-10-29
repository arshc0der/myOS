/* selftest_fs.c - tests for block devices, partitions, VFS and the filesystems. */
#include "kernel.h"
#include "vfs.h"
#include "blockdev.h"
#include "heap.h"
#include "kstring.h"
#include "kerror.h"
#include "kprintf.h"

#define T TEST_CHECK

static void fill_pattern(uint8_t *b, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)((i * 31 + seed * 17 + (i >> 8)) & 0xFF);
}

static bool same_pattern(const uint8_t *b, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++)
        if (b[i] != (uint8_t)((i * 31 + seed * 17 + (i >> 8)) & 0xFF))
            return false;
    return true;
}

static int count_entries(const char *path)
{
    struct dirent de;
    int n = 0;

    while (vfs_readdir(path, (uint32_t)n, &de) == 0)
        n++;
    return n;
}

static bool has_entry(const char *path, const char *name)
{
    struct dirent de;

    for (uint32_t i = 0; vfs_readdir(path, i, &de) == 0; i++)
        if (strcmp(de.name, name) == 0)
            return true;
    return false;
}

/* Tests that must pass on every filesystem mounted at `root`. */
static void fs_suite(const char *fsname, const char *root)
{
    char msg[80], path[96];
    char buf[64];

#define NAME(s) (ksnprintf(msg, sizeof msg, "%s: %s", fsname, s), msg)
#define P(rel)  (ksnprintf(path, sizeof path, "%s/%s", root, rel), path)

    T(count_entries(root) == 0, NAME("starts empty"));

    T(vfs_write_file(P("hello.txt"), "Hello, MyOS!", 12) == 0, NAME("write_file"));
    memset(buf, 0, sizeof buf);
    T(vfs_read_file(P("hello.txt"), buf, sizeof buf) == 12 &&
      strcmp(buf, "Hello, MyOS!") == 0, NAME("read_file returns the data"));

    struct vfs_stat st;
    T(vfs_stat(P("hello.txt"), &st) == 0 && st.size == 12 && st.type == VFS_FILE,
      NAME("stat reports size and type"));

    T(vfs_append_file(P("hello.txt"), " More.", 6) == 0, NAME("append"));
    memset(buf, 0, sizeof buf);
    vfs_read_file(P("hello.txt"), buf, sizeof buf);
    T(strcmp(buf, "Hello, MyOS! More.") == 0, NAME("append data is correct"));

    T(vfs_mkdir(P("docs")) == 0, NAME("mkdir"));
    T(vfs_mkdir(P("docs")) == -EEXIST, NAME("mkdir twice gives EEXIST"));
    T(vfs_write_file(P("docs/a.txt"), "AAA", 3) == 0, NAME("file inside a directory"));
    T(vfs_write_file(P("docs/b.txt"), "BBBB", 4) == 0, NAME("second file"));
    T(count_entries(P("docs")) == 2 && has_entry(P("docs"), "a.txt"), NAME("readdir lists entries"));
    T(vfs_stat(P("docs"), &st) == 0 && st.type == VFS_DIR, NAME("directory type"));
    T(vfs_unlink(P("docs")) == -ENOTEMPTY, NAME("unlink non-empty dir gives ENOTEMPTY"));
    T(vfs_stat(P("nothing"), &st) == -ENOENT, NAME("missing file gives ENOENT"));
    T(vfs_stat(P("hello.txt/x"), &st) == -ENOTDIR, NAME("file used as directory gives ENOTDIR"));

    /* seek / partial read / overwrite in the middle */
    int fd = vfs_open(P("hello.txt"), O_RDWR);
    T(fd >= 0, NAME("open read/write"));
    T(vfs_seek(fd, 7, SEEK_SET) == 7, NAME("seek"));
    memset(buf, 0, sizeof buf);
    T(vfs_read(fd, buf, 4) == 4 && strncmp(buf, "MyOS", 4) == 0, NAME("read after seek"));
    vfs_seek(fd, 0, SEEK_SET);
    T(vfs_write(fd, "J", 1) == 1, NAME("overwrite one byte"));
    vfs_seek(fd, 0, SEEK_SET);
    memset(buf, 0, sizeof buf);
    vfs_read(fd, buf, 5);
    T(strncmp(buf, "Jello", 5) == 0, NAME("overwrite is visible"));
    T(vfs_seek(fd, 0, SEEK_END) == 18, NAME("seek to end gives file size"));
    T(vfs_ftruncate(fd, 5) == 0 && vfs_seek(fd, 0, SEEK_END) == 5, NAME("truncate shrinks"));
    vfs_close(fd);

    /* two handles on one file see each other's changes */
    int a = vfs_open(P("shared"), O_RDWR | O_CREAT);
    int b = vfs_open(P("shared"), O_RDWR);
    vfs_write(a, "12345", 5);
    struct vfs_stat sb2;
    T(b >= 0 && vfs_fstat(b, &sb2) == 0 && sb2.size == 5, NAME("two handles share size"));
    vfs_close(a);
    vfs_close(b);

    /* a big file (needs indirect blocks on disk filesystems) */
    size_t big = 150 * 1024;
    uint8_t *data = kmalloc(big);
    uint8_t *back = kmalloc(big);
    if (data && back) {
        fill_pattern(data, big, 7);
        T(vfs_write_file(P("big.bin"), data, big) == 0, NAME("write 150 KiB file"));
        memset(back, 0, big);
        T(vfs_read_file(P("big.bin"), back, big) == (ssize_t)big, NAME("read it back (length)"));
        T(same_pattern(back, big, 7), NAME("big file content is intact"));
        T(vfs_stat(P("big.bin"), &st) == 0 && st.size == big, NAME("big file size"));
        T(vfs_unlink(P("big.bin")) == 0, NAME("delete big file"));
    }
    kfree(data);
    kfree(back);

    /* sparse write: a hole reads as zeros */
    fd = vfs_open(P("sparse"), O_RDWR | O_CREAT);
    vfs_seek(fd, 5000, SEEK_SET);
    vfs_write(fd, "X", 1);
    vfs_seek(fd, 4000, SEEK_SET);
    memset(buf, 0xFF, sizeof buf);
    vfs_read(fd, buf, 8);
    T(buf[0] == 0 && buf[7] == 0, NAME("hole reads as zeros"));
    vfs_close(fd);

    /* delete a file that is still open is refused or deferred, never corrupt */
    fd = vfs_open(P("a_open"), O_RDWR | O_CREAT);
    vfs_write(fd, "keep", 4);
    int ur = vfs_unlink(P("a_open"));
    T(ur == 0 || ur == -EBUSY, NAME("unlink of an open file is safe"));
    memset(buf, 0, sizeof buf);
    vfs_seek(fd, 0, SEEK_SET);
    T(vfs_read(fd, buf, 4) == 4 && strncmp(buf, "keep", 4) == 0, NAME("open file still readable"));
    vfs_close(fd);
    vfs_unlink(P("a_open"));

    /* O_TRUNC, O_APPEND */
    vfs_write_file(P("t.txt"), "long long text", 14);
    fd = vfs_open(P("t.txt"), O_WRONLY | O_TRUNC);
    vfs_write(fd, "ab", 2);
    vfs_close(fd);
    T(vfs_stat(P("t.txt"), &st) == 0 && st.size == 2, NAME("O_TRUNC empties the file"));

    /* many files in one directory (directory grows past one block) */
    int created = 0;
    for (int i = 0; i < 45; i++) {
        char nm[24];
        ksnprintf(nm, sizeof nm, "docs/f%02d", i);
        if (vfs_create(P(nm)) == 0)
            created++;
    }
    T(created == 45 && count_entries(P("docs")) == 47, NAME("directory with 47 entries"));
    bool ok = true;
    for (int i = 0; i < 45; i++) {
        char nm[24];
        ksnprintf(nm, sizeof nm, "docs/f%02d", i);
        ok &= vfs_unlink(P(nm)) == 0;
    }
    T(ok && count_entries(P("docs")) == 2, NAME("delete them all"));

    T(vfs_unlink(P("docs/a.txt")) == 0 && vfs_unlink(P("docs/b.txt")) == 0, NAME("unlink files"));
    T(vfs_unlink(P("docs")) == 0, NAME("unlink empty directory"));
    T(vfs_unlink(P("docs")) == -ENOENT, NAME("unlink twice gives ENOENT"));

    /* name handling */
    char longname[80];
    memset(longname, 'n', 70);
    longname[70] = 0;
    T(vfs_create(P(longname)) < 0, NAME("too long name rejected"));

#undef NAME
#undef P
}

void selftest_fs(void)
{
    char buf[64];
    struct vfs_stat st;

    /* ---- paths ---- */
    char out[VFS_PATH_MAX];
    vfs_chdir("/");
    T(vfs_normalize("/a/./b//c/../d", out, sizeof out) == 0 && strcmp(out, "/a/b/d") == 0,
      "path normalize");
    T(vfs_normalize("/..", out, sizeof out) == 0 && strcmp(out, "/") == 0, "'..' stops at root");
    T(vfs_mkdir("/tmp/dir1") == 0 && vfs_chdir("/tmp/dir1") == 0 &&
      strcmp(vfs_getcwd(), "/tmp/dir1") == 0, "chdir");
    T(vfs_write_file("rel.txt", "r", 1) == 0 && vfs_stat("/tmp/dir1/rel.txt", &st) == 0,
      "relative paths use the current directory");
    T(vfs_stat("../dir1/rel.txt", &st) == 0, "'..' in a path");
    vfs_chdir("/");
    vfs_unlink("/tmp/dir1/rel.txt");
    vfs_unlink("/tmp/dir1");

    /* ---- devfs ---- */
    int fd = vfs_open("/dev/zero", O_RDONLY);
    memset(buf, 1, sizeof buf);
    T(fd >= 0 && vfs_read(fd, buf, 8) == 8 && buf[0] == 0 && buf[7] == 0, "/dev/zero");
    vfs_close(fd);
    fd = vfs_open("/dev/null", O_WRONLY);
    T(fd >= 0 && vfs_write(fd, "x", 1) == 1, "/dev/null accepts writes");
    vfs_close(fd);
    T(vfs_stat("/dev/ram0", &st) == 0 && st.type == VFS_BLOCK, "block devices appear in /dev");

    /* ---- ramfs on /tmp ---- */
    T(vfs_mount(NULL, "/tmp", "ramfs") == 0, "mount ramfs on /tmp");
    fs_suite("ramfs", "/tmp");
    T(vfs_umount("/tmp") == 0, "umount ramfs");

    /* ---- MyFS on a RAM disk ---- */
    T(ramdisk_create("tdisk", MiB(2)) == 0, "create 2 MiB RAM disk");
    struct blockdev *d = blockdev_find("tdisk");
    T(d && d->sector_count == 4096, "RAM disk has 4096 sectors");
    uint8_t sec[SECTOR_SIZE], sec2[SECTOR_SIZE];
    fill_pattern(sec, sizeof sec, 3);
    T(blockdev_write(d, 10, 1, sec) == 0 && blockdev_read(d, 10, 1, sec2) == 0 &&
      memcmp(sec, sec2, sizeof sec) == 0, "block write/read round trip");
    T(blockdev_read(d, 4096, 1, sec2) == -ERANGE, "read past the end gives ERANGE");
    T(blockdev_write_bytes(d, 1000, "unaligned", 9) == 0 &&
      blockdev_read_bytes(d, 1000, buf, 9) == 0 && strncmp(buf, "unaligned", 9) == 0,
      "unaligned byte access");

    T(vfs_mount("tdisk", "/mnt", "myfs") < 0, "mounting an unformatted disk fails");
    T(vfs_mkfs("myfs", "tdisk") == 0, "mkfs myfs");
    T(vfs_mount("tdisk", "/mnt", "myfs") == 0, "mount myfs on /mnt");
    T(vfs_mount("tdisk", "/mnt", "myfs") == -EBUSY, "cannot mount twice at one place");
    fs_suite("myfs", "/mnt");

    /* persistence: write, unmount, mount again, read */
    T(vfs_write_file("/mnt/persist.txt", "still here", 10) == 0, "write before umount");
    T(vfs_mkdir("/mnt/keep") == 0 && vfs_write_file("/mnt/keep/x", "deep", 4) == 0, "nested data");
    fd = vfs_open("/mnt/persist.txt", O_RDONLY);
    T(vfs_umount("/mnt") == -EBUSY, "umount refused while a file is open");
    vfs_close(fd);
    T(vfs_umount("/mnt") == 0, "umount myfs");
    T(vfs_stat("/mnt/persist.txt", &st) == -ENOENT, "files vanish from the tree after umount");
    T(vfs_mount("tdisk", "/mnt", "myfs") == 0, "remount");
    memset(buf, 0, sizeof buf);
    T(vfs_read_file("/mnt/persist.txt", buf, sizeof buf) == 10 && strcmp(buf, "still here") == 0,
      "data survived unmount/mount");
    memset(buf, 0, sizeof buf);
    T(vfs_read_file("/mnt/keep/x", buf, sizeof buf) == 4 && strcmp(buf, "deep") == 0,
      "nested data survived");
    vfs_unlink("/mnt/keep/x");
    vfs_unlink("/mnt/keep");
    vfs_unlink("/mnt/persist.txt");

    /* fill the disk until it is full: must fail cleanly with ENOSPC */
    uint8_t *chunk = kmalloc(KiB(64));
    if (chunk) {
        memset(chunk, 0x5A, KiB(64));
        int r = 0, files = 0;
        for (int i = 0; i < 40 && r == 0; i++) {
            char nm[24];
            ksnprintf(nm, sizeof nm, "/mnt/fill%02d", i);
            r = vfs_write_file(nm, chunk, KiB(64));
            if (r == 0)
                files++;
        }
        T(r == -ENOSPC && files > 20, "filling the disk ends with ENOSPC");
        for (int i = 0; i < 40; i++) {
            char nm[24];
            ksnprintf(nm, sizeof nm, "/mnt/fill%02d", i);
            vfs_unlink(nm);
        }
        T(vfs_write_file("/mnt/after", "ok", 2) == 0, "space is reusable after deleting");
        vfs_unlink("/mnt/after");
        kfree(chunk);
    }
    T(vfs_umount("/mnt") == 0, "umount again");
    ramdisk_destroy("tdisk");

    /* ---- partitions ---- */
    T(ramdisk_create("pdisk", MiB(2)) == 0, "RAM disk for partitions");
    d = blockdev_find("pdisk");
    T(partition_write_mbr(d, 64, 2000, 0x83) == 0, "write an MBR");
    T(partition_scan(d) == 1 && blockdev_find("pdisk1") != NULL, "partition_scan finds pdisk1");
    struct blockdev *p = blockdev_find("pdisk1");
    T(p && p->sector_count == 2000, "partition size");
    fill_pattern(sec, sizeof sec, 9);
    blockdev_write(p, 0, 1, sec);
    blockdev_read(d, 64, 1, sec2);
    T(memcmp(sec, sec2, sizeof sec) == 0, "partition maps to the right disk sectors");
    T(vfs_mkfs("myfs", "pdisk1") == 0 && vfs_mount("pdisk1", "/mnt", "myfs") == 0,
      "myfs inside a partition");
    T(vfs_write_file("/mnt/p.txt", "in partition", 12) == 0, "write inside a partition");
    vfs_umount("/mnt");
    blockdev_unregister(blockdev_find("pdisk1"));
    ramdisk_destroy("pdisk");

    /* ---- real ATA disk (only present on IDE machines) ---- */
    struct blockdev *hd = blockdev_find("hdb");
    if (hd && hd->sector_count >= 2048) {
        fill_pattern(sec, sizeof sec, 5);
        T(blockdev_write(hd, 100, 1, sec) == 0 && blockdev_read(hd, 100, 1, sec2) == 0 &&
          memcmp(sec, sec2, sizeof sec) == 0, "ATA sector write/read");
        T(vfs_mkfs("myfs", "hdb") == 0 && vfs_mount("hdb", "/mnt", "myfs") == 0, "myfs on the ATA disk");
        T(vfs_write_file("/mnt/disk.txt", "on a real disk", 14) == 0, "write to ATA disk");
        vfs_umount("/mnt");
        T(vfs_mount("hdb", "/mnt", "myfs") == 0, "remount ATA disk");
        memset(buf, 0, sizeof buf);
        T(vfs_read_file("/mnt/disk.txt", buf, sizeof buf) == 14 && strcmp(buf, "on a real disk") == 0,
          "ATA data persisted");
        vfs_umount("/mnt");
    }
}
