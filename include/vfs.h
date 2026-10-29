/*
 * vfs.h - the Virtual File System.
 *
 * The VFS gives every filesystem the same shape, so programs can say
 * open("/disk/notes.txt") without knowing whether the file lives in RAM, on
 * an ATA disk or in your own filesystem. It does path lookup, mount points
 * and file descriptors; a *filesystem driver* only has to know how to
 * handle files and directories inside ONE filesystem.
 *
 * ==================== HOW TO WRITE YOUR OWN FILESYSTEM ====================
 *
 *  1. Fill in a `struct fs_driver` (name + mount + optional mkfs/unmount).
 *  2. Fill in a `struct node_ops` (read, write, lookup, create, ...).
 *  3. Register it with  REGISTER_FS(my_driver);
 *  4. From the shell:   mkfs myfs ram0   then   mount ram0 /mnt myfs
 *
 *  See src/fs/myfs.c (a complete example) and src/user/studentfs.c (the
 *  skeleton for you to fill in). docs/WRITING_A_FILESYSTEM.md explains
 *  every step.
 * ==========================================================================
 */
#ifndef VFS_H
#define VFS_H
#include "types.h"
#include "blockdev.h"
#include "list.h"

#define VFS_NAME_MAX   59           /* longest file name (without the NUL) */
#define VFS_PATH_MAX   256
#define VFS_MAX_MOUNTS 8

/* node types */
#define VFS_FILE  1
#define VFS_DIR   2
#define VFS_CHAR  3                 /* character device (console, null ...) */
#define VFS_BLOCK 4                 /* block device node                    */

/* open() flags */
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_ACCMODE 0x0003
#define O_CREAT   0x0040
#define O_TRUNC   0x0200
#define O_APPEND  0x0400

/* seek origins */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct vfs_node;
struct vfs_mount;

struct dirent {
    char     name[VFS_NAME_MAX + 1];
    uint32_t ino;
    uint32_t type;
    uint32_t size;
};

struct vfs_stat {
    uint32_t ino;
    uint32_t type;
    uint32_t size;
    uint32_t mode;
};

/*
 * Things you can do to a file or directory (a "node").
 * Any function you do not need may be left NULL: the VFS then returns
 * -ENOSYS to the caller.
 */
struct node_ops {
    /* Read up to len bytes starting at byte offset `off`.
     * Return bytes read (0 at end of file) or a negative error. */
    ssize_t (*read)(struct vfs_node *n, void *buf, size_t len, uint32_t off);

    /* Write len bytes at `off`, growing the file if needed.
     * Return bytes written or a negative error. */
    ssize_t (*write)(struct vfs_node *n, const void *buf, size_t len, uint32_t off);

    /* DIRECTORY: find `name` inside dir. On success return 0 and set *out
     * to a node from vfs_node_alloc(). Return -ENOENT if it does not exist. */
    int (*lookup)(struct vfs_node *dir, const char *name, struct vfs_node **out);

    /* DIRECTORY: create a file (type VFS_FILE) or sub-directory (VFS_DIR). */
    int (*create)(struct vfs_node *dir, const char *name, uint32_t type,
                  struct vfs_node **out);

    /* DIRECTORY: remove a file, or an EMPTY directory (-ENOTEMPTY if not). */
    int (*unlink)(struct vfs_node *dir, const char *name);

    /* DIRECTORY: fill *out with the index-th entry (0,1,2,...).
     * Return 0, or -ENOENT when there are no more entries. */
    int (*readdir)(struct vfs_node *dir, uint32_t index, struct dirent *out);

    /* Change a file's size (cut or extend with zeros). */
    int (*truncate)(struct vfs_node *n, uint32_t size);

    /* Called when the last user of this node let go: free node->priv. */
    void (*release)(struct vfs_node *n);
};

/* An open file or directory in memory. */
struct vfs_node {
    uint32_t ino;                   /* inode number, unique in this filesystem */
    uint32_t type;                  /* VFS_FILE, VFS_DIR, ...                  */
    uint32_t size;                  /* size in bytes (keep it up to date!)     */
    uint32_t mode;                  /* permission bits (free to use)           */
    const struct node_ops *ops;
    struct vfs_mount *mnt;          /* which mounted filesystem we belong to   */
    void *priv;                     /* your own data                           */
    int refcount;                   /* managed by the VFS                      */
    struct list_head cache;         /* VFS internal: all live nodes            */
};

/* A mounted filesystem. */
struct vfs_mount {
    bool used;
    char path[VFS_PATH_MAX];                /* where it is mounted    */
    const struct fs_driver *driver;
    struct blockdev *dev;                   /* may be NULL (ramfs...) */
    struct vfs_node *root;                  /* set by your mount()    */
    void *priv;                             /* your filesystem state  */
    /* the directory this is mounted on, as (filesystem, inode) */
    struct vfs_mount *parent_mnt;
    uint32_t parent_ino;
    int open_count;
};

/* A filesystem type. */
struct fs_driver {
    const char *name;                       /* "myfs", "ramfs" ...    */
    /* Attach to `dev` (NULL for RAM-only filesystems). Must set mnt->root
     * (a node from vfs_node_alloc) and may set mnt->priv. Return 0 or -errno. */
    int  (*mount)(struct vfs_mount *mnt, struct blockdev *dev);
    /* Flush everything and free mnt->priv. */
    void (*unmount)(struct vfs_mount *mnt);
    /* Format `dev` with an empty filesystem. Optional. */
    int  (*mkfs)(struct blockdev *dev);
};

/* Make your driver known to the kernel (works at link time, no init call). */
#define REGISTER_FS(drv) \
    static const struct fs_driver *const __fs_reg_##drv \
        __attribute__((section(".fs_drivers"), used)) = &(drv)

/* ---- helpers for filesystem authors ---- */

/* Create a node with refcount 1. The VFS frees it (after calling
 * ops->release) when nobody uses it any more. */
struct vfs_node *vfs_node_alloc(struct vfs_mount *mnt, uint32_t ino,
                                uint32_t type, uint32_t size,
                                const struct node_ops *ops, void *priv);
void vfs_node_get(struct vfs_node *n);
void vfs_node_put(struct vfs_node *n);

/* ---- using the filesystem (kernel code, shell, tests) ---- */

void vfs_init(void);                        /* mounts ramfs on "/" and devfs on /dev */

int  vfs_mount(const char *devname, const char *path, const char *fstype);
int  vfs_umount(const char *path);
int  vfs_mkfs(const char *fstype, const char *devname);
const struct fs_driver *vfs_find_driver(const char *name);
void vfs_for_each_driver(void (*cb)(const struct fs_driver *, void *), void *ctx);
void vfs_for_each_mount(void (*cb)(const struct vfs_mount *, void *), void *ctx);

/* Paths: absolute ("/a/b") or relative to the current task's directory. */
int  vfs_stat(const char *path, struct vfs_stat *st);
int  vfs_mkdir(const char *path);
int  vfs_create(const char *path);          /* empty file */
int  vfs_unlink(const char *path);          /* file or empty directory */
int  vfs_readdir(const char *path, uint32_t index, struct dirent *out);
int  vfs_chdir(const char *path);
const char *vfs_getcwd(void);
int  vfs_normalize(const char *path, char *out, size_t size);

/* File descriptors (per task). Return fd / byte count, or -errno. */
int     vfs_open(const char *path, int flags);
int     vfs_close(int fd);
ssize_t vfs_read(int fd, void *buf, size_t len);
ssize_t vfs_write(int fd, const void *buf, size_t len);
int32_t vfs_seek(int fd, int32_t off, int whence);
int     vfs_fstat(int fd, struct vfs_stat *st);
int     vfs_ftruncate(int fd, uint32_t size);

/* Whole-file shortcuts. */
ssize_t vfs_read_file(const char *path, void *buf, size_t len);
int     vfs_write_file(const char *path, const void *buf, size_t len); /* create/replace */
int     vfs_append_file(const char *path, const void *buf, size_t len);

/* Device file system (src/fs/devfs.c) */
struct chardev_ops {
    ssize_t (*read)(void *buf, size_t len);
    ssize_t (*write)(const void *buf, size_t len);
};
int devfs_register_chardev(const char *name, const struct chardev_ops *ops);

#endif
