/*
 * vfs.c - path lookup, mount table, file descriptors.
 *
 * One mutex (vfs_lock) protects the whole VFS, so filesystem drivers do not
 * need to worry about two tasks calling them at the same time.
 */
#include "vfs.h"
#include "sched.h"
#include "sync.h"
#include "heap.h"
#include "kstring.h"
#include "kerror.h"
#include "log.h"

extern const struct fs_driver *const __start_fs_drivers[];
extern const struct fs_driver *const __stop_fs_drivers[];

struct file {
    struct vfs_node *node;
    uint32_t pos;
    int flags;
};

static struct vfs_mount mounts[VFS_MAX_MOUNTS];
static struct mutex vfs_mutex = MUTEX_INIT(vfs_mutex);
static struct vfs_node *root_node;
static LIST_HEAD(node_cache);               /* every live node (inode cache) */

/* ---------- locking (the boot/idle task never blocks) ---------- */

static void lock(void)
{
    if (sched_running() && task_current()->pid != 0)
        mutex_lock(&vfs_mutex);
}

static void unlock(void)
{
    if (sched_running() && task_current()->pid != 0)
        mutex_unlock(&vfs_mutex);
}

/* ---------- nodes ---------- */

struct vfs_node *vfs_node_alloc(struct vfs_mount *mnt, uint32_t ino,
                                uint32_t type, uint32_t size,
                                const struct node_ops *ops, void *priv)
{
    struct vfs_node *n = kzalloc(sizeof *n);

    if (!n)
        return NULL;
    n->mnt = mnt;
    n->ino = ino;
    n->type = type;
    n->size = size;
    n->ops = ops;
    n->priv = priv;
    n->refcount = 1;
    list_add(&n->cache, &node_cache);
    return n;
}

void vfs_node_get(struct vfs_node *n)
{
    n->refcount++;
}

void vfs_node_put(struct vfs_node *n)
{
    if (!n)
        return;
    if (--n->refcount > 0)
        return;
    list_del(&n->cache);
    if (n->ops && n->ops->release)
        n->ops->release(n);
    kfree(n);
}

/* ---------- drivers ---------- */

const struct fs_driver *vfs_find_driver(const char *name)
{
    for (const struct fs_driver *const *p = __start_fs_drivers;
         p < __stop_fs_drivers; p++)
        if (strcmp((*p)->name, name) == 0)
            return *p;
    return NULL;
}

void vfs_for_each_driver(void (*cb)(const struct fs_driver *, void *), void *ctx)
{
    for (const struct fs_driver *const *p = __start_fs_drivers;
         p < __stop_fs_drivers; p++)
        cb(*p, ctx);
}

void vfs_for_each_mount(void (*cb)(const struct vfs_mount *, void *), void *ctx)
{
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used)
            cb(&mounts[i], ctx);
}

/* ---------- path handling ---------- */

/* Turn `path` (absolute or relative to cwd) into a clean absolute path:
 * no ".", no "..", no double slashes. */
int vfs_normalize(const char *path, char *out, size_t size)
{
    char tmp[VFS_PATH_MAX];
    const char *cwd = task_current()->cwd;
    size_t len = 0;

    if (!path || !*path)
        return -EINVAL;
    if (path[0] == '/') {
        tmp[0] = '\0';
    } else {
        strlcpy(tmp, cwd, sizeof tmp);
        len = strlen(tmp);
        if (len + 1 < sizeof tmp && (len == 0 || tmp[len - 1] != '/')) {
            tmp[len++] = '/';
            tmp[len] = '\0';
        }
    }
    if (len + strlen(path) >= sizeof tmp)
        return -ENAMETOOLONG;
    strcat(tmp, path);

    char res[VFS_PATH_MAX];
    size_t rl = 0;
    const char *p = tmp;

    while (*p) {
        while (*p == '/')
            p++;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t cl = (size_t)(p - s);
        if (cl == 0)
            break;
        if (cl == 1 && s[0] == '.')
            continue;
        if (cl == 2 && s[0] == '.' && s[1] == '.') {
            while (rl > 0 && res[rl - 1] != '/')
                rl--;
            if (rl > 0)
                rl--;                       /* drop the slash itself */
            continue;
        }
        if (cl > VFS_NAME_MAX)
            return -ENAMETOOLONG;
        if (rl + cl + 2 >= sizeof res)
            return -ENAMETOOLONG;
        res[rl++] = '/';
        memcpy(res + rl, s, cl);
        rl += cl;
    }
    if (rl == 0)
        res[rl++] = '/';
    res[rl] = '\0';
    if (rl >= size)
        return -ENAMETOOLONG;
    memcpy(out, res, rl + 1);
    return 0;
}

/*
 * Make sure there is only ONE live node per (filesystem, inode number).
 * A driver's lookup() may hand back a fresh node for a file that is
 * already open; we swap it for the existing one so that size and state
 * are shared between all openers.
 */
static struct vfs_node *canonical(struct vfs_node *n)
{
    struct list_head *p;

    list_for_each(p, &node_cache) {
        struct vfs_node *o = list_entry(p, struct vfs_node, cache);
        if (o != n && o->mnt == n->mnt && o->ino == n->ino) {
            vfs_node_get(o);
            vfs_node_put(n);
            return o;
        }
    }
    return n;
}

/* If `n` is a mount point, return the root of the filesystem mounted there. */
static struct vfs_node *cross_mount(struct vfs_node *n)
{
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        struct vfs_mount *m = &mounts[i];
        if (m->used && m->parent_mnt == n->mnt && m->parent_ino == n->ino &&
            m->root && !(m->parent_mnt == NULL)) {
            vfs_node_get(m->root);
            vfs_node_put(n);
            return m->root;
        }
    }
    return n;
}

/* Look up an already-normalized absolute path. Returns a referenced node. */
static int resolve_abs(const char *abs, struct vfs_node **out)
{
    struct vfs_node *cur = root_node;
    char name[VFS_NAME_MAX + 1];
    const char *p = abs;

    if (!cur)
        return -ENOENT;
    vfs_node_get(cur);
    cur = cross_mount(cur);

    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        size_t n = 0;
        while (p[n] && p[n] != '/')
            n++;
        memcpy(name, p, n);
        name[n] = '\0';
        p += n;

        if (cur->type != VFS_DIR) {
            vfs_node_put(cur);
            return -ENOTDIR;
        }
        if (!cur->ops || !cur->ops->lookup) {
            vfs_node_put(cur);
            return -ENOSYS;
        }
        struct vfs_node *next = NULL;
        int r = cur->ops->lookup(cur, name, &next);
        vfs_node_put(cur);
        if (r < 0)
            return r;
        cur = cross_mount(canonical(next));
    }
    *out = cur;
    return 0;
}

static int resolve(const char *path, struct vfs_node **out)
{
    char abs[VFS_PATH_MAX];
    int r = vfs_normalize(path, abs, sizeof abs);

    if (r < 0)
        return r;
    return resolve_abs(abs, out);
}

/* Split "/a/b/c" into the directory node "/a/b" and the name "c". */
static int resolve_parent(const char *path, struct vfs_node **dir, char *name)
{
    char abs[VFS_PATH_MAX];
    int r = vfs_normalize(path, abs, sizeof abs);

    if (r < 0)
        return r;
    if (strcmp(abs, "/") == 0)
        return -EINVAL;
    char *slash = strrchr(abs, '/');
    strlcpy(name, slash + 1, VFS_NAME_MAX + 1);
    if (slash == abs)
        slash[1] = '\0';
    else
        *slash = '\0';
    r = resolve_abs(abs, dir);
    if (r < 0)
        return r;
    if ((*dir)->type != VFS_DIR) {
        vfs_node_put(*dir);
        return -ENOTDIR;
    }
    return 0;
}

/* ---------- mounting ---------- */

static struct vfs_mount *find_mount_by_path(const char *abs)
{
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && strcmp(mounts[i].path, abs) == 0)
            return &mounts[i];
    return NULL;
}

static int do_mount(const char *devname, const char *path, const char *fstype)
{
    const struct fs_driver *drv = vfs_find_driver(fstype);
    struct blockdev *dev = NULL;
    char abs[VFS_PATH_MAX];
    struct vfs_mount *m = NULL;
    struct vfs_node *mp = NULL;
    int r;

    if (!drv)
        return -ENODEV;
    if (devname && strcmp(devname, "none") != 0) {
        dev = blockdev_find(devname);
        if (!dev)
            return -ENODEV;
    }
    if ((r = vfs_normalize(path, abs, sizeof abs)) < 0)
        return r;
    if (find_mount_by_path(abs))
        return -EBUSY;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (!mounts[i].used) {
            m = &mounts[i];
            break;
        }
    if (!m)
        return -ENOMEM;

    memset(m, 0, sizeof *m);
    m->driver = drv;
    m->dev = dev;
    strlcpy(m->path, abs, sizeof m->path);

    if (strcmp(abs, "/") != 0) {                    /* find the mount point */
        if ((r = resolve_abs(abs, &mp)) < 0)
            return r;
        if (mp->type != VFS_DIR) {
            vfs_node_put(mp);
            return -ENOTDIR;
        }
        m->parent_mnt = mp->mnt;
        m->parent_ino = mp->ino;
        vfs_node_put(mp);
    }
    if ((r = drv->mount(m, dev)) < 0)
        return r;
    if (!m->root) {
        if (drv->unmount)
            drv->unmount(m);
        return -EIO;
    }
    m->used = true;
    if (strcmp(abs, "/") == 0)
        root_node = m->root;
    return 0;
}

int vfs_mount(const char *devname, const char *path, const char *fstype)
{
    lock();
    int r = do_mount(devname, path, fstype);
    unlock();
    return r;
}

int vfs_umount(const char *path)
{
    char abs[VFS_PATH_MAX];
    int r;

    lock();
    if ((r = vfs_normalize(path, abs, sizeof abs)) < 0)
        goto out;
    struct vfs_mount *m = find_mount_by_path(abs);
    if (!m) {
        r = -EINVAL;
        goto out;
    }
    if (strcmp(abs, "/") == 0 || m->open_count > 0) {
        r = -EBUSY;
        goto out;
    }
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)        /* something mounted inside? */
        if (mounts[i].used && mounts[i].parent_mnt == m) {
            r = -EBUSY;
            goto out;
        }
    if (m->root && m->root->refcount > 1) {      /* someone still uses it */
        r = -EBUSY;
        goto out;
    }
    if (m->driver->unmount)
        m->driver->unmount(m);          /* driver flushes while root is valid */
    vfs_node_put(m->root);
    m->used = false;
    r = 0;
out:
    unlock();
    return r;
}

int vfs_mkfs(const char *fstype, const char *devname)
{
    const struct fs_driver *drv = vfs_find_driver(fstype);
    struct blockdev *dev = blockdev_find(devname);

    if (!drv || !dev)
        return -ENODEV;
    if (!drv->mkfs)
        return -ENOSYS;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && mounts[i].dev == dev)
            return -EBUSY;
    lock();
    int r = drv->mkfs(dev);
    unlock();
    return r;
}

/* ---------- path-level operations ---------- */

static void fill_stat(struct vfs_stat *st, const struct vfs_node *n)
{
    st->ino = n->ino;
    st->type = n->type;
    st->size = n->size;
    st->mode = n->mode;
}

int vfs_stat(const char *path, struct vfs_stat *st)
{
    struct vfs_node *n;

    lock();
    int r = resolve(path, &n);
    if (r == 0) {
        fill_stat(st, n);
        vfs_node_put(n);
    }
    unlock();
    return r;
}

static int create_node(const char *path, uint32_t type)
{
    struct vfs_node *dir, *n = NULL;
    char name[VFS_NAME_MAX + 1];
    int r = resolve_parent(path, &dir, name);

    if (r < 0)
        return r;
    if (!dir->ops || !dir->ops->create) {
        r = -ENOSYS;
    } else {
        struct vfs_node *existing = NULL;
        if (dir->ops->lookup && dir->ops->lookup(dir, name, &existing) == 0) {
            vfs_node_put(existing);
            r = -EEXIST;
        } else {
            r = dir->ops->create(dir, name, type, &n);
            if (r == 0)
                vfs_node_put(canonical(n));
        }
    }
    vfs_node_put(dir);
    return r;
}

int vfs_mkdir(const char *path)
{
    lock();
    int r = create_node(path, VFS_DIR);
    unlock();
    return r;
}

int vfs_create(const char *path)
{
    lock();
    int r = create_node(path, VFS_FILE);
    unlock();
    return r;
}

int vfs_unlink(const char *path)
{
    struct vfs_node *dir;
    char name[VFS_NAME_MAX + 1];

    lock();
    int r = resolve_parent(path, &dir, name);
    if (r == 0) {
        if (!dir->ops || !dir->ops->unlink) {
            r = -ENOSYS;
        } else {
            char abs[VFS_PATH_MAX];
            vfs_normalize(path, abs, sizeof abs);
            if (find_mount_by_path(abs))
                r = -EBUSY;                 /* it is a mount point */
            else
                r = dir->ops->unlink(dir, name);
        }
        vfs_node_put(dir);
    }
    unlock();
    return r;
}

int vfs_readdir(const char *path, uint32_t index, struct dirent *out)
{
    struct vfs_node *n;

    lock();
    int r = resolve(path, &n);
    if (r == 0) {
        if (n->type != VFS_DIR)
            r = -ENOTDIR;
        else if (!n->ops || !n->ops->readdir)
            r = -ENOSYS;
        else
            r = n->ops->readdir(n, index, out);
        vfs_node_put(n);
    }
    unlock();
    return r;
}

int vfs_chdir(const char *path)
{
    char abs[VFS_PATH_MAX];
    struct vfs_node *n;

    lock();
    int r = vfs_normalize(path, abs, sizeof abs);
    if (r == 0)
        r = resolve_abs(abs, &n);
    if (r == 0) {
        if (n->type != VFS_DIR)
            r = -ENOTDIR;
        else
            strlcpy(task_current()->cwd, abs, TASK_CWD_MAX);
        vfs_node_put(n);
    }
    unlock();
    return r;
}

const char *vfs_getcwd(void)
{
    return task_current()->cwd;
}

/* ---------- file descriptors ---------- */

static int fd_alloc(struct task *t)
{
    for (int i = 0; i < TASK_MAX_FDS; i++)
        if (!t->fds[i])
            return i;
    return -EMFILE;
}

static struct file *fd_get(int fd)
{
    struct task *t = task_current();

    if (fd < 0 || fd >= TASK_MAX_FDS)
        return NULL;
    return t->fds[fd];
}

int vfs_open(const char *path, int flags)
{
    struct task *t = task_current();
    struct vfs_node *n = NULL;
    int r, fd;

    lock();
    if ((fd = fd_alloc(t)) < 0) {
        r = fd;
        goto out;
    }
    r = resolve(path, &n);
    if (r == -ENOENT && (flags & O_CREAT)) {
        if ((r = create_node(path, VFS_FILE)) < 0)
            goto out;
        r = resolve(path, &n);
    }
    if (r < 0)
        goto out;
    if (n->type == VFS_DIR && (flags & O_ACCMODE) != O_RDONLY) {
        vfs_node_put(n);
        r = -EISDIR;
        goto out;
    }
    if ((flags & O_TRUNC) && n->type == VFS_FILE && (flags & O_ACCMODE) != O_RDONLY) {
        if (!n->ops || !n->ops->truncate) {
            vfs_node_put(n);
            r = -ENOSYS;
            goto out;
        }
        if ((r = n->ops->truncate(n, 0)) < 0) {
            vfs_node_put(n);
            goto out;
        }
    }
    struct file *f = kzalloc(sizeof *f);
    if (!f) {
        vfs_node_put(n);
        r = -ENOMEM;
        goto out;
    }
    f->node = n;
    f->flags = flags;
    n->mnt->open_count++;
    t->fds[fd] = f;
    r = fd;
out:
    unlock();
    return r;
}

static void close_file(struct file *f)
{
    f->node->mnt->open_count--;
    vfs_node_put(f->node);
    kfree(f);
}

int vfs_close(int fd)
{
    struct task *t = task_current();
    int r = 0;

    lock();
    struct file *f = fd_get(fd);
    if (!f) {
        r = -EBADF;
    } else {
        t->fds[fd] = NULL;
        close_file(f);
    }
    unlock();
    return r;
}

/* Called when a task exits (see sched.c). */
void vfs_task_cleanup(struct task *t)
{
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (t->fds[i]) {
            close_file(t->fds[i]);
            t->fds[i] = NULL;
        }
    }
}

ssize_t vfs_read(int fd, void *buf, size_t len)
{
    lock();
    struct file *f = fd_get(fd);
    ssize_t r;

    if (!f)
        r = -EBADF;
    else if ((f->flags & O_ACCMODE) == O_WRONLY)
        r = -EBADF;
    else if (f->node->type == VFS_DIR)
        r = -EISDIR;
    else if (!f->node->ops || !f->node->ops->read)
        r = -ENOSYS;
    else {
        r = f->node->ops->read(f->node, buf, len, f->pos);
        if (r > 0)
            f->pos += (uint32_t)r;
    }
    unlock();
    return r;
}

ssize_t vfs_write(int fd, const void *buf, size_t len)
{
    lock();
    struct file *f = fd_get(fd);
    ssize_t r;

    if (!f)
        r = -EBADF;
    else if ((f->flags & O_ACCMODE) == O_RDONLY)
        r = -EBADF;
    else if (f->node->type == VFS_DIR)
        r = -EISDIR;
    else if (!f->node->ops || !f->node->ops->write)
        r = -ENOSYS;
    else {
        if (f->flags & O_APPEND)
            f->pos = f->node->size;
        r = f->node->ops->write(f->node, buf, len, f->pos);
        if (r > 0)
            f->pos += (uint32_t)r;
    }
    unlock();
    return r;
}

int32_t vfs_seek(int fd, int32_t off, int whence)
{
    lock();
    struct file *f = fd_get(fd);
    int32_t r;

    if (!f) {
        r = -EBADF;
    } else {
        int64_t base = whence == SEEK_SET ? 0 :
                       whence == SEEK_CUR ? (int64_t)f->pos :
                       whence == SEEK_END ? (int64_t)f->node->size : -1;
        int64_t pos = base + off;
        if (base < 0 || pos < 0 || pos > 0x7FFFFFFF)
            r = -EINVAL;
        else
            r = (int32_t)(f->pos = (uint32_t)pos);
    }
    unlock();
    return r;
}

int vfs_fstat(int fd, struct vfs_stat *st)
{
    lock();
    struct file *f = fd_get(fd);
    int r = f ? 0 : -EBADF;

    if (f)
        fill_stat(st, f->node);
    unlock();
    return r;
}

int vfs_ftruncate(int fd, uint32_t size)
{
    lock();
    struct file *f = fd_get(fd);
    int r;

    if (!f)
        r = -EBADF;
    else if (!f->node->ops || !f->node->ops->truncate)
        r = -ENOSYS;
    else
        r = f->node->ops->truncate(f->node, size);
    unlock();
    return r;
}

/* ---------- whole-file helpers ---------- */

ssize_t vfs_read_file(const char *path, void *buf, size_t len)
{
    int fd = vfs_open(path, O_RDONLY);
    ssize_t total = 0;

    if (fd < 0)
        return fd;
    while ((size_t)total < len) {
        ssize_t n = vfs_read(fd, (uint8_t *)buf + total, len - (size_t)total);
        if (n < 0) {
            vfs_close(fd);
            return n;
        }
        if (n == 0)
            break;
        total += n;
    }
    vfs_close(fd);
    return total;
}

static int write_all(int fd, const void *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = vfs_write(fd, (const uint8_t *)buf + done, len - done);
        if (n < 0)
            return (int)n;
        if (n == 0)
            return -EIO;
        done += (size_t)n;
    }
    return 0;
}

int vfs_write_file(const char *path, const void *buf, size_t len)
{
    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    int r;

    if (fd < 0)
        return fd;
    r = write_all(fd, buf, len);
    vfs_close(fd);
    return r;
}

int vfs_append_file(const char *path, const void *buf, size_t len)
{
    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_APPEND);
    int r;

    if (fd < 0)
        return fd;
    r = write_all(fd, buf, len);
    vfs_close(fd);
    return r;
}

/* ---------- start-up ---------- */

void vfs_init(void)
{
    memset(mounts, 0, sizeof mounts);
    root_node = NULL;
    if (vfs_mount(NULL, "/", "ramfs") < 0) {
        log_error("vfs", "cannot mount the root filesystem");
        return;
    }
    vfs_mkdir("/dev");
    vfs_mkdir("/tmp");
    vfs_mkdir("/mnt");
    if (vfs_mount(NULL, "/dev", "devfs") < 0)
        log_error("vfs", "cannot mount devfs");
    log_info("vfs", "root is ramfs; /dev is devfs; /tmp and /mnt are empty directories");
}
