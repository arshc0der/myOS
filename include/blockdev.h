/*
 * blockdev.h - block devices (disks).
 *
 * A block device is anything you can read and write in fixed-size chunks
 * called sectors (512 bytes): a RAM disk, a hard disk, a partition.
 * Filesystems sit on top of block devices and never talk to hardware.
 *
 *     struct blockdev *d = blockdev_find("hdb");
 *     uint8_t buf[512];
 *     blockdev_read(d, 0, 1, buf);        // read sector 0
 */
#ifndef BLOCKDEV_H
#define BLOCKDEV_H
#include "types.h"
#include "list.h"

#define SECTOR_SIZE 512

struct blockdev {
    char     name[16];                  /* "ram0", "hda", "hda1" ...    */
    const char *kind;                   /* "ram", "ata", "partition"    */
    uint32_t sector_count;              /* size in sectors              */
    bool     readonly;
    /* Driver functions. Return 0 on success or a negative error code. */
    int (*read)(struct blockdev *d, uint32_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *d, uint32_t lba, uint32_t count, const void *buf);
    void    *priv;                      /* driver's private data        */
    char     model[41];                 /* e.g. disk model name         */
    struct list_head node;
};

int  blockdev_register(struct blockdev *d);         /* takes ownership */
void blockdev_unregister(struct blockdev *d);
struct blockdev *blockdev_find(const char *name);
void blockdev_for_each(void (*cb)(struct blockdev *, void *), void *ctx);
int  blockdev_count(void);

/* Checked wrappers (range and read-only checks, then call the driver). */
int  blockdev_read(struct blockdev *d, uint32_t lba, uint32_t count, void *buf);
int  blockdev_write(struct blockdev *d, uint32_t lba, uint32_t count, const void *buf);
/* Byte-granular helpers (do read-modify-write when not sector aligned). */
int  blockdev_read_bytes(struct blockdev *d, uint64_t off, void *buf, uint32_t len);
int  blockdev_write_bytes(struct blockdev *d, uint64_t off, const void *buf, uint32_t len);
static inline uint64_t blockdev_size(const struct blockdev *d)
{
    return (uint64_t)d->sector_count * SECTOR_SIZE;
}

/* ---- RAM disks (src/fs/ramdisk.c) ---- */
int  ramdisk_create(const char *name, uint32_t size_bytes);
/* Wrap memory that already exists (e.g. a GRUB module). Not freed. */
int  ramdisk_create_from_memory(const char *name, void *mem, uint32_t size_bytes);
int  ramdisk_destroy(const char *name);

/* ---- ATA hard disks (src/drivers/ata.c) ---- */
void ata_init(void);

/* ---- partitions (src/fs/partition.c) ---- */
/* Read the MBR of `d` and register hda1, hda2, ... for each partition. */
int  partition_scan(struct blockdev *d);
/* Write a fresh MBR with one partition covering [start, start+count). */
int  partition_write_mbr(struct blockdev *d, uint32_t start, uint32_t count, uint8_t type);

#endif
