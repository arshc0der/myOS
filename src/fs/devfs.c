/*
 * devfs.c - the /dev directory: devices appear as files.
 *
 *   /dev/null     discards writes, reads as empty
 *   /dev/zero     reads as zero bytes
 *   /dev/random   pseudo-random bytes
 *   /dev/console  writing prints on the screen
 *   /dev/ram0     every block device shows up here as raw bytes
 *
 * Add your own with devfs_register_chardev().
 */
#include "vfs.h"
#include "blockdev.h"
#include "console.h"
#include "heap.h"
#include "kstring.h"
#include "kerror.h"
#include "timer.h"

#define MAX_CHARDEVS 16

static struct {
    char name[VFS_NAME_MAX + 1];
    const struct chardev_ops *ops;
} chars[MAX_CHARDEVS];
static int nchars;

static const struct node_ops dev_dir_ops, dev_char_ops, dev_block_ops;

/* ---- built-in character devices ---- */

static ssize_t null_read(void *b, size_t n)  { (void)b; (void)n; return 0; }
static ssize_t null_write(const void *b, size_t n) { (void)b; return (ssize_t)n; }
static ssize_t zero_read(void *b, size_t n) { memset(b, 0, n); return (ssize_t)n; }

static uint32_t rng_state = 0x2545F491;
static ssize_t random_read(void *b, size_t n)
{
    uint8_t *p = b;
    rng_state ^= timer_ticks() * 2654435761u;
    for (size_t i = 0; i < n; i++) {
        rng_state ^= rng_state << 13;           /* xorshift32 */
        rng_state ^= rng_state >> 17;
        rng_state ^= rng_state << 5;
        p[i] = (uint8_t)rng_state;
    }
    return (ssize_t)n;
}

static ssize_t console_write_dev(const void *b, size_t n)
{
    const char *p = b;
    for (size_t i = 0; i < n; i++)
        console_putc(p[i]);
    return (ssize_t)n;
}

static const struct chardev_ops null_ops = { null_read, null_write };
static const struct chardev_ops zero_ops = { zero_read, null_write };
static const struct chardev_ops random_ops = { random_read, null_write };
static const struct chardev_ops console_ops = { null_read, console_write_dev };

int devfs_register_chardev(const char *name, const struct chardev_ops *ops)
{
    if (nchars >= MAX_CHARDEVS)
        return -ENOMEM;
    for (int i = 0; i < nchars; i++)
        if (strcmp(chars[i].name, name) == 0)
            return -EEXIST;
    strlcpy(chars[nchars].name, name, sizeof chars[nchars].name);
    chars[nchars].ops = ops;
    nchars++;
    return 0;
}

/* inode numbers: 1 = root, 100+i = char device i, 1000+k = block device k */

static int block_index(const char *name)
{
    struct blockdev *d = blockdev_find(name);
    if (!d)
        return -1;
    int k = 0;
    struct list_head *p;
    (void)p;
    /* a stable number: position in the registry is not exposed, so hash it */
    for (const char *c = name; *c; c++)
        k = k * 31 + *c;
    return k & 0xFFFFF;
}

static int dev_lookup(struct vfs_node *dir, const char *name, struct vfs_node **out)
{
    for (int i = 0; i < nchars; i++) {
        if (strcmp(chars[i].name, name) == 0) {
            *out = vfs_node_alloc(dir->mnt, 100 + (uint32_t)i, VFS_CHAR, 0,
                                  &dev_char_ops, (void *)chars[i].ops);
            return *out ? 0 : -ENOMEM;
        }
    }
    struct blockdev *d = blockdev_find(name);
    if (d) {
        *out = vfs_node_alloc(dir->mnt, 1000 + (uint32_t)block_index(name),
                              VFS_BLOCK, (uint32_t)MIN(blockdev_size(d), 0xFFFFFFFFu),
                              &dev_block_ops, d);
        return *out ? 0 : -ENOMEM;
    }
    return -ENOENT;
}

struct readdir_ctx {
    uint32_t index, seen;
    struct dirent *out;
    bool found;
};

static void readdir_block(struct blockdev *d, void *arg)
{
    struct readdir_ctx *c = arg;

    if (c->found)
        return;
    if (c->seen++ == c->index) {
        strlcpy(c->out->name, d->name, sizeof c->out->name);
        c->out->ino = 1000 + (uint32_t)block_index(d->name);
        c->out->type = VFS_BLOCK;
        c->out->size = (uint32_t)MIN(blockdev_size(d), 0xFFFFFFFFu);
        c->found = true;
    }
}

static int dev_readdir(struct vfs_node *dir, uint32_t index, struct dirent *out)
{
    (void)dir;
    if (index < (uint32_t)nchars) {
        strlcpy(out->name, chars[index].name, sizeof out->name);
        out->ino = 100 + index;
        out->type = VFS_CHAR;
        out->size = 0;
        return 0;
    }
    struct readdir_ctx c = { index - (uint32_t)nchars, 0, out, false };
    blockdev_for_each(readdir_block, &c);
    return c.found ? 0 : -ENOENT;
}

static ssize_t chr_read(struct vfs_node *n, void *buf, size_t len, uint32_t off)
{
    (void)off;
    const struct chardev_ops *o = n->priv;
    return o->read ? o->read(buf, len) : -ENOSYS;
}

static ssize_t chr_write(struct vfs_node *n, const void *buf, size_t len, uint32_t off)
{
    (void)off;
    const struct chardev_ops *o = n->priv;
    return o->write ? o->write(buf, len) : -ENOSYS;
}

static ssize_t blk_read(struct vfs_node *n, void *buf, size_t len, uint32_t off)
{
    struct blockdev *d = n->priv;
    uint64_t size = blockdev_size(d);

    if (off >= size)
        return 0;
    if (len > size - off)
        len = (size_t)(size - off);
    int r = blockdev_read_bytes(d, off, buf, (uint32_t)len);
    return r < 0 ? r : (ssize_t)len;
}

static ssize_t blk_write(struct vfs_node *n, const void *buf, size_t len, uint32_t off)
{
    struct blockdev *d = n->priv;
    uint64_t size = blockdev_size(d);

    if (off >= size)
        return -ENOSPC;
    if (len > size - off)
        len = (size_t)(size - off);
    int r = blockdev_write_bytes(d, off, buf, (uint32_t)len);
    return r < 0 ? r : (ssize_t)len;
}

static const struct node_ops dev_dir_ops = {
    .lookup = dev_lookup,
    .readdir = dev_readdir,
};
static const struct node_ops dev_char_ops = { .read = chr_read, .write = chr_write };
static const struct node_ops dev_block_ops = { .read = blk_read, .write = blk_write };

static int devfs_mount(struct vfs_mount *m, struct blockdev *dev)
{
    (void)dev;
    if (nchars == 0) {
        devfs_register_chardev("null", &null_ops);
        devfs_register_chardev("zero", &zero_ops);
        devfs_register_chardev("random", &random_ops);
        devfs_register_chardev("console", &console_ops);
    }
    m->root = vfs_node_alloc(m, 1, VFS_DIR, 0, &dev_dir_ops, NULL);
    return m->root ? 0 : -ENOMEM;
}

static const struct fs_driver devfs_driver = {
    .name = "devfs",
    .mount = devfs_mount,
};
REGISTER_FS(devfs_driver);
