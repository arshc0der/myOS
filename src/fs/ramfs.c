/*
 * ramfs.c - a filesystem that keeps everything in RAM.
 *
 * This is the SIMPLEST complete filesystem in MyOS: read it first. Files
 * are heap buffers, directories are linked lists of names. It shows the
 * pattern every driver follows:
 *
 *     mount()  -> create a root node
 *     lookup() -> find a name in a directory and make a vfs_node for it
 *     create() / unlink() / readdir() / read() / write() / truncate()
 *
 * Nothing is stored on a disk, so everything is lost at power-off.
 */
#include "vfs.h"
#include "heap.h"
#include "kstring.h"
#include "kerror.h"
#include "list.h"

#define RAMFS_MAX_FILE MiB(16)

struct rnode {
    uint32_t ino;
    uint32_t type;
    uint8_t *data;
    uint32_t size, cap;
    struct list_head children;          /* directories: list of rentry */
    int      users;                     /* live vfs_nodes pointing here */
    bool     unlinked;                  /* removed from its directory   */
};

struct rentry {
    struct list_head link;
    char name[VFS_NAME_MAX + 1];
    struct rnode *node;
};

struct ramfs {
    uint32_t next_ino;
};

static const struct node_ops ramfs_ops;

static struct rnode *rnode_new(struct ramfs *fs, uint32_t type)
{
    struct rnode *r = kzalloc(sizeof *r);

    if (!r)
        return NULL;
    r->ino = fs->next_ino++;
    r->type = type;
    list_init(&r->children);
    return r;
}

static void rnode_free(struct rnode *r)
{
    kfree(r->data);
    kfree(r);
}

/* Free a whole tree (used at unmount). */
static void tree_free(struct rnode *r)
{
    struct list_head *p, *n;

    list_for_each_safe(p, n, &r->children) {
        struct rentry *e = list_entry(p, struct rentry, link);
        list_del(&e->link);
        tree_free(e->node);
        kfree(e);
    }
    rnode_free(r);
}

static struct vfs_node *vnode(struct vfs_mount *m, struct rnode *r)
{
    struct vfs_node *n = vfs_node_alloc(m, r->ino, r->type, r->size,
                                        &ramfs_ops, r);
    if (n)
        r->users++;
    return n;
}

static void ram_release(struct vfs_node *n)
{
    struct rnode *r = n->priv;

    if (--r->users == 0 && r->unlinked)
        rnode_free(r);
}

static ssize_t ram_read(struct vfs_node *n, void *buf, size_t len, uint32_t off)
{
    struct rnode *r = n->priv;

    if (off >= r->size)
        return 0;
    if (len > r->size - off)
        len = r->size - off;
    memcpy(buf, r->data + off, len);
    return (ssize_t)len;
}

static int ensure_cap(struct rnode *r, uint32_t need)
{
    if (need <= r->cap)
        return 0;
    if (need > RAMFS_MAX_FILE)
        return -EFBIG;
    uint32_t cap = r->cap ? r->cap : 64;
    while (cap < need)
        cap *= 2;
    uint8_t *p = krealloc(r->data, cap);
    if (!p)
        return -ENOMEM;
    r->data = p;
    r->cap = cap;
    return 0;
}

static ssize_t ram_write(struct vfs_node *n, const void *buf, size_t len, uint32_t off)
{
    struct rnode *r = n->priv;

    if ((uint64_t)off + len > RAMFS_MAX_FILE)
        return -EFBIG;
    int rc = ensure_cap(r, off + (uint32_t)len);
    if (rc < 0)
        return rc;
    if (off > r->size)
        memset(r->data + r->size, 0, off - r->size);    /* hole -> zeros */
    memcpy(r->data + off, buf, len);
    if (off + len > r->size)
        r->size = off + (uint32_t)len;
    n->size = r->size;
    return (ssize_t)len;
}

static int ram_truncate(struct vfs_node *n, uint32_t size)
{
    struct rnode *r = n->priv;

    if (size > r->size) {
        int rc = ensure_cap(r, size);
        if (rc < 0)
            return rc;
        memset(r->data + r->size, 0, size - r->size);
    }
    r->size = size;
    n->size = size;
    return 0;
}

static struct rentry *find_entry(struct rnode *dir, const char *name)
{
    struct list_head *p;

    list_for_each(p, &dir->children) {
        struct rentry *e = list_entry(p, struct rentry, link);
        if (strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

static int ram_lookup(struct vfs_node *dir, const char *name, struct vfs_node **out)
{
    struct rentry *e = find_entry(dir->priv, name);

    if (!e)
        return -ENOENT;
    *out = vnode(dir->mnt, e->node);
    return *out ? 0 : -ENOMEM;
}

static int ram_create(struct vfs_node *dir, const char *name, uint32_t type,
                      struct vfs_node **out)
{
    struct rnode *d = dir->priv;
    struct ramfs *fs = dir->mnt->priv;

    if (type != VFS_FILE && type != VFS_DIR)
        return -EINVAL;
    if (strlen(name) > VFS_NAME_MAX)
        return -ENAMETOOLONG;
    if (find_entry(d, name))
        return -EEXIST;

    struct rnode *r = rnode_new(fs, type);
    struct rentry *e = kzalloc(sizeof *e);
    if (!r || !e) {
        if (r)
            rnode_free(r);
        kfree(e);
        return -ENOMEM;
    }
    strlcpy(e->name, name, sizeof e->name);
    e->node = r;
    list_add_tail(&e->link, &d->children);
    *out = vnode(dir->mnt, r);
    return *out ? 0 : -ENOMEM;
}

static int ram_unlink(struct vfs_node *dir, const char *name)
{
    struct rentry *e = find_entry(dir->priv, name);

    if (!e)
        return -ENOENT;
    if (e->node->type == VFS_DIR && !list_empty(&e->node->children))
        return -ENOTEMPTY;
    list_del(&e->link);
    e->node->unlinked = true;
    if (e->node->users == 0)
        rnode_free(e->node);
    kfree(e);
    return 0;
}

static int ram_readdir(struct vfs_node *dir, uint32_t index, struct dirent *out)
{
    struct rnode *d = dir->priv;
    struct list_head *p;
    uint32_t i = 0;

    list_for_each(p, &d->children) {
        if (i++ == index) {
            struct rentry *e = list_entry(p, struct rentry, link);
            strlcpy(out->name, e->name, sizeof out->name);
            out->ino = e->node->ino;
            out->type = e->node->type;
            out->size = e->node->size;
            return 0;
        }
    }
    return -ENOENT;
}

static const struct node_ops ramfs_ops = {
    .read = ram_read,
    .write = ram_write,
    .lookup = ram_lookup,
    .create = ram_create,
    .unlink = ram_unlink,
    .readdir = ram_readdir,
    .truncate = ram_truncate,
    .release = ram_release,
};

static int ramfs_mount(struct vfs_mount *m, struct blockdev *dev)
{
    (void)dev;
    struct ramfs *fs = kzalloc(sizeof *fs);
    if (!fs)
        return -ENOMEM;
    fs->next_ino = 1;
    struct rnode *root = rnode_new(fs, VFS_DIR);
    if (!root) {
        kfree(fs);
        return -ENOMEM;
    }
    m->priv = fs;
    m->root = vnode(m, root);
    if (!m->root) {
        rnode_free(root);
        kfree(fs);
        return -ENOMEM;
    }
    return 0;
}

static void ramfs_unmount(struct vfs_mount *m)
{
    /* The VFS still holds m->root while we run. Free everything below the
     * root now; the VFS then drops the root node, whose release() frees
     * the root rnode itself (it is marked unlinked). */
    struct rnode *root = m->root->priv;
    struct list_head *p, *n;

    list_for_each_safe(p, n, &root->children) {
        struct rentry *e = list_entry(p, struct rentry, link);
        list_del(&e->link);
        tree_free(e->node);
        kfree(e);
    }
    root->unlinked = true;
    kfree(m->priv);
    m->priv = NULL;
}

static const struct fs_driver ramfs_driver = {
    .name = "ramfs",
    .mount = ramfs_mount,
    .unmount = ramfs_unmount,
};
REGISTER_FS(ramfs_driver);
