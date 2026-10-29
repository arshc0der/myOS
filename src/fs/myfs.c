/*
 * myfs.c - MyFS: a small, complete on-disk filesystem.
 *
 * This is the reference example for writing your own filesystem. Read the
 * layout, then follow how each VFS operation maps onto disk blocks.
 *
 * Disk layout (block size = 1024 bytes = 2 sectors):
 *
 *   block 0              superblock   (sizes and where everything is)
 *   block 1 ...          block bitmap (1 bit per block: 1 = in use)
 *   after the bitmap     inode table  (64-byte inodes, 16 per block)
 *   after the inodes     data blocks  (file contents and directories)
 *
 * An INODE describes one file or directory: its type, size and up to 12
 * direct block numbers plus one "indirect" block holding 256 more. So the
 * largest file is (12 + 256) * 1024 = 268 KiB. (Exercise: add a double
 * indirect block to make files bigger.)
 *
 * A DIRECTORY is just a file whose data is an array of 32-byte entries:
 * { inode number (4 bytes), name (28 bytes) }. Inode number 0 = unused slot.
 *
 * All changes are written to disk immediately ("write-through"), so there
 * is nothing to flush at unmount except the superblock counters.
 */
#include "vfs.h"
#include "blockdev.h"
#include "heap.h"
#include "bitmap.h"
#include "kstring.h"
#include "kerror.h"
#include "log.h"

#define MYFS_MAGIC     0x4D594653u          /* "MYFS" */
#define MYFS_VERSION   1
#define BS             1024u
#define SECT_PER_BLK   (BS / SECTOR_SIZE)
#define NDIRECT        12
#define NINDIRECT      (BS / 4)
#define MAX_FILE_BLKS  (NDIRECT + NINDIRECT)
#define INODE_SIZE     64
#define INODES_PER_BLK (BS / INODE_SIZE)
#define DIRENT_SIZE    32
#define DIRENTS_PER_BLK (BS / DIRENT_SIZE)
#define MYFS_NAME_MAX  27
#define ROOT_INO       1
#define T_FREE 0
#define T_FILE 1
#define T_DIR  2

struct myfs_super {
    uint32_t magic, version;
    uint32_t block_count, inode_count;
    uint32_t bitmap_start, bitmap_blocks;
    uint32_t inode_start, inode_blocks;
    uint32_t data_start;
    uint32_t free_blocks, free_inodes;
    uint32_t root_ino;
} PACKED;

struct myfs_inode {
    uint16_t type;
    uint16_t links;
    uint32_t size;
    uint32_t direct[NDIRECT];
    uint32_t indirect;
    uint32_t mtime;
} PACKED;

struct myfs_dirent {
    uint32_t ino;
    char     name[28];
} PACKED;

_Static_assert(sizeof(struct myfs_inode) == INODE_SIZE, "inode size");
_Static_assert(sizeof(struct myfs_dirent) == DIRENT_SIZE, "dirent size");
_Static_assert(sizeof(struct myfs_super) <= BS, "superblock size");

#define OPEN_SLOTS 64

struct myfs {
    struct blockdev *dev;
    struct myfs_super sb;
    uint8_t *bitmap;                    /* in-memory copy of the block bitmap */
    uint32_t alloc_hint;
    struct { uint32_t ino; int users; } open[OPEN_SLOTS];  /* in-use inodes */
};

static const struct node_ops myfs_ops;

/* ------------------------------------------------------------------ */
/* Block I/O: a "block" is 2 sectors.                                  */
/* ------------------------------------------------------------------ */

static int blk_rd(struct myfs *fs, uint32_t b, void *buf)
{
    return blockdev_read(fs->dev, b * SECT_PER_BLK, SECT_PER_BLK, buf);
}

static int blk_wr(struct myfs *fs, uint32_t b, const void *buf)
{
    return blockdev_write(fs->dev, b * SECT_PER_BLK, SECT_PER_BLK, buf);
}

static int sb_write(struct myfs *fs)
{
    uint8_t blk[BS];

    memset(blk, 0, sizeof blk);
    memcpy(blk, &fs->sb, sizeof fs->sb);
    return blk_wr(fs, 0, blk);
}

/* ------------------------------------------------------------------ */
/* Free-block bitmap                                                   */
/* ------------------------------------------------------------------ */

static int bitmap_flush_block(struct myfs *fs, uint32_t bit)
{
    uint32_t idx = bit / (BS * 8);
    return blk_wr(fs, fs->sb.bitmap_start + idx, fs->bitmap + idx * BS);
}

static int alloc_block(struct myfs *fs, uint32_t *out)
{
    uint32_t total = fs->sb.block_count;
    uint32_t start = fs->alloc_hint < fs->sb.data_start ? fs->sb.data_start
                                                         : fs->alloc_hint;

    for (uint32_t n = 0; n < total; n++) {
        uint32_t b = start + n;
        if (b >= total)
            b = fs->sb.data_start + (b - total);
        if (b < fs->sb.data_start || b >= total)
            continue;
        if (!bitmap_test((uint32_t *)fs->bitmap, b)) {
            uint8_t zero[BS];
            memset(zero, 0, sizeof zero);
            bitmap_set((uint32_t *)fs->bitmap, b);
            int r = bitmap_flush_block(fs, b);
            if (r < 0)
                return r;
            if ((r = blk_wr(fs, b, zero)) < 0)      /* new blocks start zeroed */
                return r;
            fs->sb.free_blocks--;
            fs->alloc_hint = b + 1;
            *out = b;
            return 0;
        }
    }
    return -ENOSPC;
}

static void free_block(struct myfs *fs, uint32_t b)
{
    if (b < fs->sb.data_start || b >= fs->sb.block_count)
        return;
    bitmap_clear((uint32_t *)fs->bitmap, b);
    bitmap_flush_block(fs, b);
    fs->sb.free_blocks++;
    if (b < fs->alloc_hint)
        fs->alloc_hint = b;
}

/* ------------------------------------------------------------------ */
/* Inodes                                                              */
/* ------------------------------------------------------------------ */

static int inode_read(struct myfs *fs, uint32_t ino, struct myfs_inode *out)
{
    uint8_t blk[BS];

    if (ino == 0 || ino > fs->sb.inode_count)
        return -EINVAL;
    int r = blk_rd(fs, fs->sb.inode_start + (ino - 1) / INODES_PER_BLK, blk);
    if (r < 0)
        return r;
    memcpy(out, blk + ((ino - 1) % INODES_PER_BLK) * INODE_SIZE, INODE_SIZE);
    return 0;
}

static int inode_write(struct myfs *fs, uint32_t ino, const struct myfs_inode *in)
{
    uint8_t blk[BS];
    uint32_t b = fs->sb.inode_start + (ino - 1) / INODES_PER_BLK;
    int r = blk_rd(fs, b, blk);

    if (r < 0)
        return r;
    memcpy(blk + ((ino - 1) % INODES_PER_BLK) * INODE_SIZE, in, INODE_SIZE);
    return blk_wr(fs, b, blk);
}

static int inode_alloc(struct myfs *fs, uint16_t type, uint32_t *out)
{
    uint8_t blk[BS];

    for (uint32_t b = 0; b < fs->sb.inode_blocks; b++) {
        int r = blk_rd(fs, fs->sb.inode_start + b, blk);
        if (r < 0)
            return r;
        for (uint32_t i = 0; i < INODES_PER_BLK; i++) {
            struct myfs_inode *ip = (struct myfs_inode *)(blk + i * INODE_SIZE);
            uint32_t ino = b * INODES_PER_BLK + i + 1;
            if (ino > fs->sb.inode_count)
                return -ENOSPC;
            if (ip->type == T_FREE) {
                memset(ip, 0, sizeof *ip);
                ip->type = type;
                ip->links = type == T_DIR ? 2 : 1;
                if ((r = blk_wr(fs, fs->sb.inode_start + b, blk)) < 0)
                    return r;
                fs->sb.free_inodes--;
                *out = ino;
                return 0;
            }
        }
    }
    return -ENOSPC;
}

/*
 * Which disk block holds byte range number `fbi` (file block index) of this
 * file? If `alloc` is true, allocate it when missing. *blk = 0 means "hole"
 * (never written; reads as zeros). Changes the inode in memory: the caller
 * must write the inode back.
 */
static int bmap(struct myfs *fs, struct myfs_inode *ino, uint32_t fbi,
                bool alloc, uint32_t *blk)
{
    int r;

    if (fbi < NDIRECT) {
        if (!ino->direct[fbi] && alloc) {
            uint32_t nb;
            if ((r = alloc_block(fs, &nb)) < 0)
                return r;
            ino->direct[fbi] = nb;
        }
        *blk = ino->direct[fbi];
        return 0;
    }
    fbi -= NDIRECT;
    if (fbi >= NINDIRECT)
        return -EFBIG;
    if (!ino->indirect) {
        if (!alloc) {
            *blk = 0;
            return 0;
        }
        uint32_t nb;
        if ((r = alloc_block(fs, &nb)) < 0)
            return r;
        ino->indirect = nb;
    }
    uint32_t table[NINDIRECT];
    if ((r = blk_rd(fs, ino->indirect, table)) < 0)
        return r;
    if (!table[fbi] && alloc) {
        if ((r = alloc_block(fs, &table[fbi])) < 0)
            return r;
        if ((r = blk_wr(fs, ino->indirect, table)) < 0)
            return r;
    }
    *blk = table[fbi];
    return 0;
}

/* Free every data block with file index >= from (and the indirect block
 * if nothing in it remains). */
static void free_blocks_from(struct myfs *fs, struct myfs_inode *ino, uint32_t from)
{
    for (uint32_t i = from; i < NDIRECT; i++) {
        if (ino->direct[i]) {
            free_block(fs, ino->direct[i]);
            ino->direct[i] = 0;
        }
    }
    if (!ino->indirect)
        return;
    uint32_t table[NINDIRECT];
    if (blk_rd(fs, ino->indirect, table) < 0)
        return;
    uint32_t start = from > NDIRECT ? from - NDIRECT : 0;
    bool any = false;
    for (uint32_t i = start; i < NINDIRECT; i++) {
        if (table[i]) {
            free_block(fs, table[i]);
            table[i] = 0;
        }
    }
    for (uint32_t i = 0; i < NINDIRECT; i++)
        if (table[i])
            any = true;
    if (any) {
        blk_wr(fs, ino->indirect, table);
    } else {
        free_block(fs, ino->indirect);
        ino->indirect = 0;
    }
}

/* ------------------------------------------------------------------ */
/* File data                                                           */
/* ------------------------------------------------------------------ */

static ssize_t myfs_read(struct vfs_node *n, void *buf, size_t len, uint32_t off)
{
    struct myfs *fs = n->mnt->priv;
    struct myfs_inode ino;
    uint8_t blk[BS];
    uint8_t *out = buf;
    int r;

    if ((r = inode_read(fs, n->ino, &ino)) < 0)
        return r;
    if (off >= ino.size)
        return 0;
    if (len > ino.size - off)
        len = ino.size - off;

    size_t done = 0;
    while (done < len) {
        uint32_t pos = off + (uint32_t)done;
        uint32_t within = pos % BS;
        size_t chunk = MIN(len - done, BS - within);
        uint32_t b;

        if ((r = bmap(fs, &ino, pos / BS, false, &b)) < 0)
            return r;
        if (b == 0) {
            memset(out + done, 0, chunk);               /* hole */
        } else {
            if ((r = blk_rd(fs, b, blk)) < 0)
                return r;
            memcpy(out + done, blk + within, chunk);
        }
        done += chunk;
    }
    return (ssize_t)done;
}

static ssize_t myfs_write(struct vfs_node *n, const void *buf, size_t len, uint32_t off)
{
    struct myfs *fs = n->mnt->priv;
    struct myfs_inode ino;
    uint8_t blk[BS];
    const uint8_t *in = buf;
    int r;

    if ((r = inode_read(fs, n->ino, &ino)) < 0)
        return r;
    if ((uint64_t)off + len > (uint64_t)MAX_FILE_BLKS * BS)
        return -EFBIG;

    size_t done = 0;
    r = 0;
    while (done < len) {
        uint32_t pos = off + (uint32_t)done;
        uint32_t within = pos % BS;
        size_t chunk = MIN(len - done, BS - within);
        uint32_t b;

        if ((r = bmap(fs, &ino, pos / BS, true, &b)) < 0)
            break;
        if (chunk == BS) {
            memcpy(blk, in + done, BS);
        } else {
            if ((r = blk_rd(fs, b, blk)) < 0)
                break;
            memcpy(blk + within, in + done, chunk);
        }
        if ((r = blk_wr(fs, b, blk)) < 0)
            break;
        done += chunk;
    }
    if (off + done > ino.size)
        ino.size = off + (uint32_t)done;
    int wr = inode_write(fs, n->ino, &ino);
    sb_write(fs);
    n->size = ino.size;
    if (done == 0)
        return r < 0 ? r : wr;
    return (ssize_t)done;
}

static int myfs_truncate(struct vfs_node *n, uint32_t size)
{
    struct myfs *fs = n->mnt->priv;
    struct myfs_inode ino;
    int r;

    if ((r = inode_read(fs, n->ino, &ino)) < 0)
        return r;
    if (size > (uint32_t)MAX_FILE_BLKS * BS)
        return -EFBIG;
    if (size < ino.size) {
        uint32_t keep = (size + BS - 1) / BS;
        free_blocks_from(fs, &ino, keep);
        if (size % BS) {                        /* zero the tail of the last block */
            uint8_t blk[BS];
            uint32_t b;
            if (bmap(fs, &ino, size / BS, false, &b) == 0 && b &&
                blk_rd(fs, b, blk) == 0) {
                memset(blk + size % BS, 0, BS - size % BS);
                blk_wr(fs, b, blk);
            }
        }
    }
    ino.size = size;
    r = inode_write(fs, n->ino, &ino);
    sb_write(fs);
    n->size = size;
    return r;
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

/* Look for `name`. On success returns 0 with the entry's inode number and
 * its position (block index + slot) so callers can modify it. */
static int dir_find(struct myfs *fs, struct myfs_inode *dir, const char *name,
                    uint32_t *ino_out, uint32_t *fbi_out, uint32_t *slot_out)
{
    uint8_t blk[BS];
    uint32_t nblk = dir->size / BS;

    for (uint32_t i = 0; i < nblk; i++) {
        uint32_t b;
        int r = bmap(fs, dir, i, false, &b);
        if (r < 0)
            return r;
        if (!b)
            continue;
        if ((r = blk_rd(fs, b, blk)) < 0)
            return r;
        struct myfs_dirent *e = (struct myfs_dirent *)blk;
        for (uint32_t s = 0; s < DIRENTS_PER_BLK; s++) {
            if (e[s].ino && strncmp(e[s].name, name, sizeof e[s].name) == 0 &&
                strlen(name) <= MYFS_NAME_MAX) {
                if (ino_out)  *ino_out = e[s].ino;
                if (fbi_out)  *fbi_out = i;
                if (slot_out) *slot_out = s;
                return 0;
            }
        }
    }
    return -ENOENT;
}

static int dir_add(struct myfs *fs, uint32_t dir_ino, struct myfs_inode *dir,
                   const char *name, uint32_t child)
{
    uint8_t blk[BS];
    uint32_t nblk = dir->size / BS;
    int r;

    for (uint32_t i = 0; i <= nblk; i++) {
        uint32_t b;
        bool grow = (i == nblk);                 /* past the end: add a block */

        if ((r = bmap(fs, dir, i, grow, &b)) < 0)
            return r;
        if (!b)
            continue;
        if (grow) {
            dir->size += BS;
            memset(blk, 0, sizeof blk);
        } else if ((r = blk_rd(fs, b, blk)) < 0) {
            return r;
        }
        struct myfs_dirent *e = (struct myfs_dirent *)blk;
        for (uint32_t s = 0; s < DIRENTS_PER_BLK; s++) {
            if (e[s].ino == 0) {
                e[s].ino = child;
                memset(e[s].name, 0, sizeof e[s].name);
                strlcpy(e[s].name, name, sizeof e[s].name);
                if ((r = blk_wr(fs, b, blk)) < 0)
                    return r;
                return inode_write(fs, dir_ino, dir);
            }
        }
    }
    return -ENOSPC;
}

static bool dir_is_empty(struct myfs *fs, struct myfs_inode *dir)
{
    uint8_t blk[BS];

    for (uint32_t i = 0; i < dir->size / BS; i++) {
        uint32_t b;
        if (bmap(fs, dir, i, false, &b) < 0 || !b)
            continue;
        if (blk_rd(fs, b, blk) < 0)
            return false;
        struct myfs_dirent *e = (struct myfs_dirent *)blk;
        for (uint32_t s = 0; s < DIRENTS_PER_BLK; s++)
            if (e[s].ino)
                return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* VFS nodes                                                           */
/* ------------------------------------------------------------------ */

/* Remember which inodes are open so unlink can refuse to delete them. */
static bool open_inc(struct myfs *fs, uint32_t ino)
{
    int freeslot = -1;

    for (int i = 0; i < OPEN_SLOTS; i++) {
        if (fs->open[i].users && fs->open[i].ino == ino) {
            fs->open[i].users++;
            return true;
        }
        if (!fs->open[i].users && freeslot < 0)
            freeslot = i;
    }
    if (freeslot < 0)
        return false;
    fs->open[freeslot].ino = ino;
    fs->open[freeslot].users = 1;
    return true;
}

static int open_users(struct myfs *fs, uint32_t ino)
{
    for (int i = 0; i < OPEN_SLOTS; i++)
        if (fs->open[i].users && fs->open[i].ino == ino)
            return fs->open[i].users;
    return 0;
}

static void myfs_release(struct vfs_node *n)
{
    struct myfs *fs = n->mnt->priv;

    if (!fs)
        return;
    for (int i = 0; i < OPEN_SLOTS; i++)
        if (fs->open[i].users && fs->open[i].ino == n->ino) {
            fs->open[i].users--;
            return;
        }
}

static struct vfs_node *make_node(struct vfs_mount *m, uint32_t ino,
                                  const struct myfs_inode *di)
{
    struct myfs *fs = m->priv;

    if (!open_inc(fs, ino))
        return NULL;
    struct vfs_node *n = vfs_node_alloc(m, ino,
                                        di->type == T_DIR ? VFS_DIR : VFS_FILE,
                                        di->size, &myfs_ops, NULL);
    if (!n) {
        for (int i = 0; i < OPEN_SLOTS; i++)
            if (fs->open[i].users && fs->open[i].ino == ino) {
                fs->open[i].users--;
                break;
            }
    }
    return n;
}

static int myfs_lookup(struct vfs_node *dirn, const char *name, struct vfs_node **out)
{
    struct myfs *fs = dirn->mnt->priv;
    struct myfs_inode dir, child;
    uint32_t cino;
    int r;

    if ((r = inode_read(fs, dirn->ino, &dir)) < 0)
        return r;
    if ((r = dir_find(fs, &dir, name, &cino, NULL, NULL)) < 0)
        return r;
    if ((r = inode_read(fs, cino, &child)) < 0)
        return r;
    *out = make_node(dirn->mnt, cino, &child);
    return *out ? 0 : -ENOMEM;
}

static int myfs_create(struct vfs_node *dirn, const char *name, uint32_t type,
                       struct vfs_node **out)
{
    struct myfs *fs = dirn->mnt->priv;
    struct myfs_inode dir, child;
    uint32_t cino;
    int r;

    if (type != VFS_FILE && type != VFS_DIR)
        return -EINVAL;
    if (strlen(name) > MYFS_NAME_MAX)
        return -ENAMETOOLONG;
    if ((r = inode_read(fs, dirn->ino, &dir)) < 0)
        return r;
    if (dir_find(fs, &dir, name, NULL, NULL, NULL) == 0)
        return -EEXIST;
    if ((r = inode_alloc(fs, type == VFS_DIR ? T_DIR : T_FILE, &cino)) < 0)
        return r;
    if ((r = dir_add(fs, dirn->ino, &dir, name, cino)) < 0) {
        memset(&child, 0, sizeof child);        /* undo the inode */
        inode_write(fs, cino, &child);
        fs->sb.free_inodes++;
        return r;
    }
    dirn->size = dir.size;
    sb_write(fs);
    if ((r = inode_read(fs, cino, &child)) < 0)
        return r;
    *out = make_node(dirn->mnt, cino, &child);
    return *out ? 0 : -ENOMEM;
}

static int myfs_unlink(struct vfs_node *dirn, const char *name)
{
    struct myfs *fs = dirn->mnt->priv;
    struct myfs_inode dir, child;
    uint32_t cino, fbi, slot, b;
    uint8_t blk[BS];
    int r;

    if ((r = inode_read(fs, dirn->ino, &dir)) < 0)
        return r;
    if ((r = dir_find(fs, &dir, name, &cino, &fbi, &slot)) < 0)
        return r;
    if (open_users(fs, cino) > 0)
        return -EBUSY;                          /* still open somewhere */
    if ((r = inode_read(fs, cino, &child)) < 0)
        return r;
    if (child.type == T_DIR && !dir_is_empty(fs, &child))
        return -ENOTEMPTY;

    free_blocks_from(fs, &child, 0);            /* give back the data blocks */
    memset(&child, 0, sizeof child);            /* type = T_FREE             */
    if ((r = inode_write(fs, cino, &child)) < 0)
        return r;
    fs->sb.free_inodes++;

    if ((r = bmap(fs, &dir, fbi, false, &b)) < 0 || (r = blk_rd(fs, b, blk)) < 0)
        return r;
    struct myfs_dirent *e = (struct myfs_dirent *)blk;
    e[slot].ino = 0;
    memset(e[slot].name, 0, sizeof e[slot].name);
    if ((r = blk_wr(fs, b, blk)) < 0)
        return r;
    return sb_write(fs);
}

static int myfs_readdir(struct vfs_node *dirn, uint32_t index, struct dirent *out)
{
    struct myfs *fs = dirn->mnt->priv;
    struct myfs_inode dir, child;
    uint8_t blk[BS];
    uint32_t seen = 0;
    int r;

    if ((r = inode_read(fs, dirn->ino, &dir)) < 0)
        return r;
    for (uint32_t i = 0; i < dir.size / BS; i++) {
        uint32_t b;
        if ((r = bmap(fs, &dir, i, false, &b)) < 0)
            return r;
        if (!b || (r = blk_rd(fs, b, blk)) < 0)
            continue;
        struct myfs_dirent *e = (struct myfs_dirent *)blk;
        for (uint32_t s = 0; s < DIRENTS_PER_BLK; s++) {
            if (!e[s].ino)
                continue;
            if (seen++ != index)
                continue;
            if ((r = inode_read(fs, e[s].ino, &child)) < 0)
                return r;
            memset(out->name, 0, sizeof out->name);
            memcpy(out->name, e[s].name, sizeof e[s].name);
            out->ino = e[s].ino;
            out->type = child.type == T_DIR ? VFS_DIR : VFS_FILE;
            out->size = child.size;
            return 0;
        }
    }
    return -ENOENT;
}

static const struct node_ops myfs_ops = {
    .read = myfs_read,
    .write = myfs_write,
    .lookup = myfs_lookup,
    .create = myfs_create,
    .unlink = myfs_unlink,
    .readdir = myfs_readdir,
    .truncate = myfs_truncate,
    .release = myfs_release,
};

/* ------------------------------------------------------------------ */
/* mkfs / mount / unmount                                              */
/* ------------------------------------------------------------------ */

static int myfs_mkfs(struct blockdev *dev)
{
    struct myfs_super sb;
    uint8_t blk[BS];
    int r;

    memset(&sb, 0, sizeof sb);
    sb.magic = MYFS_MAGIC;
    sb.version = MYFS_VERSION;
    sb.block_count = dev->sector_count / SECT_PER_BLK;
    if (sb.block_count < 16)
        return -ENOSPC;
    sb.inode_count = MIN(MAX(sb.block_count / 4, 32u), 4096u);
    sb.inode_count = (sb.inode_count + INODES_PER_BLK - 1) / INODES_PER_BLK * INODES_PER_BLK;
    sb.inode_blocks = sb.inode_count / INODES_PER_BLK;
    sb.bitmap_start = 1;
    sb.bitmap_blocks = (sb.block_count + BS * 8 - 1) / (BS * 8);
    sb.inode_start = sb.bitmap_start + sb.bitmap_blocks;
    sb.data_start = sb.inode_start + sb.inode_blocks;
    sb.root_ino = ROOT_INO;
    if (sb.data_start + 2 >= sb.block_count)
        return -ENOSPC;
    sb.free_blocks = sb.block_count - sb.data_start;
    sb.free_inodes = sb.inode_count - 1;                /* root uses one */

    /* zero the metadata area */
    memset(blk, 0, sizeof blk);
    for (uint32_t b = 0; b < sb.data_start; b++)
        if ((r = blockdev_write(dev, b * SECT_PER_BLK, SECT_PER_BLK, blk)) < 0)
            return r;

    /* bitmap: mark metadata blocks and blocks past the end as used */
    uint8_t *bm = kzalloc(sb.bitmap_blocks * BS);
    if (!bm)
        return -ENOMEM;
    bitmap_set_range((uint32_t *)bm, 0, sb.data_start);
    bitmap_set_range((uint32_t *)bm, sb.block_count,
                     sb.bitmap_blocks * BS * 8 - sb.block_count);
    for (uint32_t i = 0; i < sb.bitmap_blocks; i++)
        if ((r = blockdev_write(dev, (sb.bitmap_start + i) * SECT_PER_BLK,
                                SECT_PER_BLK, bm + i * BS)) < 0) {
            kfree(bm);
            return r;
        }
    kfree(bm);

    /* root directory inode */
    memset(blk, 0, sizeof blk);
    struct myfs_inode *root = (struct myfs_inode *)blk;
    root->type = T_DIR;
    root->links = 2;
    if ((r = blockdev_write(dev, sb.inode_start * SECT_PER_BLK, SECT_PER_BLK, blk)) < 0)
        return r;

    memset(blk, 0, sizeof blk);
    memcpy(blk, &sb, sizeof sb);
    return blockdev_write(dev, 0, SECT_PER_BLK, blk);
}

static int myfs_mount(struct vfs_mount *m, struct blockdev *dev)
{
    uint8_t blk[BS];
    int r;

    if (!dev)
        return -ENODEV;
    if ((r = blockdev_read(dev, 0, SECT_PER_BLK, blk)) < 0)
        return r;
    const struct myfs_super *sb = (const struct myfs_super *)blk;
    if (sb->magic != MYFS_MAGIC || sb->version != MYFS_VERSION) {
        log_warn("myfs", "%s: not a MyFS filesystem (run mkfs first)", dev->name);
        return -EINVAL;
    }
    if (sb->block_count * SECT_PER_BLK > dev->sector_count ||
        sb->data_start >= sb->block_count || sb->inode_blocks == 0)
        return -EINVAL;

    struct myfs *fs = kzalloc(sizeof *fs);
    if (!fs)
        return -ENOMEM;
    fs->dev = dev;
    fs->sb = *sb;
    fs->bitmap = kmalloc(sb->bitmap_blocks * BS);
    if (!fs->bitmap) {
        kfree(fs);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < sb->bitmap_blocks; i++)
        if ((r = blk_rd(fs, sb->bitmap_start + i, fs->bitmap + i * BS)) < 0) {
            kfree(fs->bitmap);
            kfree(fs);
            return r;
        }
    fs->alloc_hint = sb->data_start;
    m->priv = fs;

    struct myfs_inode root;
    if ((r = inode_read(fs, sb->root_ino, &root)) < 0 || root.type != T_DIR) {
        kfree(fs->bitmap);
        kfree(fs);
        m->priv = NULL;
        return r < 0 ? r : -EINVAL;
    }
    m->root = make_node(m, sb->root_ino, &root);
    if (!m->root) {
        kfree(fs->bitmap);
        kfree(fs);
        m->priv = NULL;
        return -ENOMEM;
    }
    return 0;
}

static void myfs_unmount(struct vfs_mount *m)
{
    struct myfs *fs = m->priv;

    sb_write(fs);
    /* The VFS drops m->root after this returns; release() must still find
     * fs, so keep it until then: it is freed by the lines below only when
     * no node needs it. */
    for (int i = 0; i < OPEN_SLOTS; i++)
        fs->open[i].users = 0;
    kfree(fs->bitmap);
    kfree(fs);
    m->priv = NULL;                         /* release() checks for NULL */
}

static const struct fs_driver myfs_driver = {
    .name = "myfs",
    .mount = myfs_mount,
    .unmount = myfs_unmount,
    .mkfs = myfs_mkfs,
};
REGISTER_FS(myfs_driver);
