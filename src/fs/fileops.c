/*
 * fileops.c - copy / move / delete / rename on top of the VFS (see fileops.h).
 * Works between any two filesystems because it only uses the VFS interface.
 */
#include "fileops.h"
#include "vfs.h"
#include "heap.h"
#include "kerror.h"
#include "kprintf.h"
#include "kstring.h"

#define MAX_DEPTH 12
#define CHUNK 4096

const char *fo_basename(const char *path)
{
    const char *s = strrchr(path, '/');

    return s ? s + 1 : path;
}

void fo_join(char *out, size_t n, const char *dir, const char *name)
{
    if (!strcmp(dir, "/"))
        ksnprintf(out, n, "/%s", name);
    else
        ksnprintf(out, n, "%s/%s", dir, name);
}

bool fo_exists(const char *path)
{
    struct vfs_stat st;

    return vfs_stat(path, &st) == 0;
}

void fo_unique_path(const char *dir, const char *base, char *out, size_t n)
{
    char name[VFS_NAME_MAX + 1];

    fo_join(out, n, dir, base);
    for (int i = 2; fo_exists(out) && i < 1000; i++) {
        ksnprintf(name, sizeof name, "%s %d", base, i);
        fo_join(out, n, dir, name);
    }
}

void fo_copy_name(const char *dir, const char *name, char *out, size_t n)
{
    char stem[VFS_NAME_MAX + 1], tmp[VFS_NAME_MAX + 1];
    const char *ext = "";
    const char *dot = strrchr(name, '.');

    if (dot && dot != name) {
        size_t l = (size_t)(dot - name);
        if (l >= sizeof stem)
            l = sizeof stem - 1;
        memcpy(stem, name, l);
        stem[l] = '\0';
        ext = dot;
    } else {
        strlcpy(stem, name, sizeof stem);
    }
    ksnprintf(tmp, sizeof tmp, "%s copy%s", stem, ext);
    fo_join(out, n, dir, tmp);
    for (int i = 2; fo_exists(out) && i < 1000; i++) {
        ksnprintf(tmp, sizeof tmp, "%s copy %d%s", stem, i, ext);
        fo_join(out, n, dir, tmp);
    }
}

int fo_new_file(const char *path)
{
    if (fo_exists(path))
        return -EEXIST;
    return vfs_create(path);
}

int fo_new_dir(const char *path)
{
    if (fo_exists(path))
        return -EEXIST;
    return vfs_mkdir(path);
}

static int copy_file(const char *src, const char *dst)
{
    char *buf = kmalloc(CHUNK);
    int in, out, rc = 0;

    if (!buf)
        return -ENOMEM;
    in = vfs_open(src, O_RDONLY);
    if (in < 0) {
        kfree(buf);
        return in;
    }
    out = vfs_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        vfs_close(in);
        kfree(buf);
        return out;
    }
    for (;;) {
        ssize_t n = vfs_read(in, buf, CHUNK);
        if (n < 0) {
            rc = (int)n;
            break;
        }
        if (n == 0)
            break;
        ssize_t w = vfs_write(out, buf, (size_t)n);
        if (w != n) {
            rc = w < 0 ? (int)w : -ENOSPC;
            break;
        }
    }
    vfs_close(in);
    vfs_close(out);
    kfree(buf);
    if (rc < 0)
        vfs_unlink(dst);                         /* do not leave a half-written copy */
    return rc;
}

static int copy_rec(const char *src, const char *dst, int depth)
{
    struct vfs_stat st;
    int rc = vfs_stat(src, &st);

    if (rc < 0)
        return rc;
    if (st.type != VFS_DIR)
        return st.type == VFS_FILE ? copy_file(src, dst) : -EINVAL;
    if (depth > MAX_DEPTH)
        return -ENAMETOOLONG;
    rc = vfs_mkdir(dst);
    if (rc < 0)
        return rc;
    struct dirent *de = kmalloc(sizeof *de);
    if (!de)
        return -ENOMEM;
    for (uint32_t i = 0; vfs_readdir(src, i, de) == 0; i++) {
        if (!strcmp(de->name, ".") || !strcmp(de->name, ".."))
            continue;
        char s[VFS_PATH_MAX], d[VFS_PATH_MAX];
        fo_join(s, sizeof s, src, de->name);
        fo_join(d, sizeof d, dst, de->name);
        rc = copy_rec(s, d, depth + 1);
        if (rc < 0)
            break;
    }
    kfree(de);
    return rc;
}

int fo_copy(const char *src, const char *dst)
{
    char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
    size_t la;

    if (vfs_normalize(src, a, sizeof a) < 0 || vfs_normalize(dst, b, sizeof b) < 0)
        return -EINVAL;
    la = strlen(a);
    if (!strcmp(a, b))
        return -EEXIST;
    if (!strncmp(b, a, la) && (b[la] == '/' || la == 1))   /* copying a folder into itself */
        return -EINVAL;
    if (fo_exists(b))
        return -EEXIST;
    return copy_rec(a, b, 0);
}

static int remove_rec(const char *path, int depth)
{
    struct vfs_stat st;
    int rc = vfs_stat(path, &st);

    if (rc < 0)
        return rc;
    if (st.type == VFS_DIR) {
        if (depth > MAX_DEPTH)
            return -ENAMETOOLONG;
        struct dirent *de = kmalloc(sizeof *de);
        if (!de)
            return -ENOMEM;
        /* always ask for entry 0: the directory shrinks as we delete */
        while (vfs_readdir(path, 0, de) == 0) {
            char c[VFS_PATH_MAX];
            if (!strcmp(de->name, ".") || !strcmp(de->name, "..")) {
                kfree(de);
                return -EIO;
            }
            fo_join(c, sizeof c, path, de->name);
            rc = remove_rec(c, depth + 1);
            if (rc < 0) {
                kfree(de);
                return rc;
            }
        }
        kfree(de);
    }
    return vfs_unlink(path);
}

int fo_remove(const char *path)
{
    char p[VFS_PATH_MAX];

    if (vfs_normalize(path, p, sizeof p) < 0 || !strcmp(p, "/"))
        return -EINVAL;
    return remove_rec(p, 0);
}

int fo_move(const char *src, const char *dst)
{
    int rc = fo_copy(src, dst);

    if (rc < 0)
        return rc;
    rc = fo_remove(src);
    if (rc < 0)
        fo_remove(dst);                           /* keep the original, drop the copy */
    return rc;
}

int fo_rename(const char *path, const char *new_name)
{
    char dir[VFS_PATH_MAX], dst[VFS_PATH_MAX];

    if (!new_name[0] || strchr(new_name, '/') || strlen(new_name) > VFS_NAME_MAX)
        return -EINVAL;
    if (vfs_normalize(path, dir, sizeof dir) < 0)
        return -EINVAL;
    char *slash = strrchr(dir, '/');
    if (!slash)
        return -EINVAL;
    if (slash == dir)
        dir[1] = '\0';
    else
        *slash = '\0';
    fo_join(dst, sizeof dst, dir, new_name);
    if (!strcmp(fo_basename(path), new_name))
        return 0;
    return fo_move(path, dst);
}
