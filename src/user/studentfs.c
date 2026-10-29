/*
 * studentfs.c - skeleton of your own filesystem, "hellofs".
 * It has one read-only file, /hello.txt. Try:
 *     mount none /hello hellofs     (then)  cat /hello/hello.txt
 * Extend it: add create/write/unlink, or store data on a block device
 * (see src/fs/myfs.c for a full disk-based example, docs/WRITING_A_FILESYSTEM.md).
 */
#include "vfs.h"
#include "kerror.h"
#include "kstring.h"

static const char hello_text[] = "Hello from my own filesystem!\n";

static ssize_t h_read(struct vfs_node *n, void *buf, size_t len, uint32_t off)
{
    (void)n;
    if (off >= sizeof hello_text - 1)
        return 0;
    size_t left = sizeof hello_text - 1 - off;
    if (len > left)
        len = left;
    memcpy(buf, hello_text + off, len);
    return (ssize_t)len;
}

static const struct node_ops file_ops = { .read = h_read };

static int h_lookup(struct vfs_node *dir, const char *name, struct vfs_node **out)
{
    if (strcmp(name, "hello.txt") != 0)
        return -ENOENT;
    *out = vfs_node_alloc(dir->mnt, 2, VFS_FILE, sizeof hello_text - 1, &file_ops, NULL);
    return *out ? 0 : -ENOMEM;
}

static int h_readdir(struct vfs_node *dir, uint32_t index, struct dirent *out)
{
    (void)dir;
    if (index != 0)
        return -ENOENT;
    strcpy(out->name, "hello.txt");
    out->ino = 2;
    out->type = VFS_FILE;
    out->size = sizeof hello_text - 1;
    return 0;
}

static const struct node_ops dir_ops = { .lookup = h_lookup, .readdir = h_readdir };

static int h_mount(struct vfs_mount *m, struct blockdev *dev)
{
    (void)dev;
    m->root = vfs_node_alloc(m, 1, VFS_DIR, 0, &dir_ops, NULL);
    return m->root ? 0 : -ENOMEM;
}

static const struct fs_driver hellofs = { .name = "hellofs", .mount = h_mount };
REGISTER_FS(hellofs);
