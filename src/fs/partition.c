/*
 * partition.c - MBR partition tables.
 *
 * Sector 0 of a disk can hold a table of up to 4 partitions. Each one is
 * exposed as its own block device ("hda1") that maps onto part of the disk.
 */
#include "blockdev.h"
#include "heap.h"
#include "kstring.h"
#include "kerror.h"
#include "log.h"

struct mbr_entry {
    uint8_t  status, chs_first[3], type, chs_last[3];
    uint32_t lba_start, sectors;
} PACKED;

struct part {
    struct blockdev *parent;
    uint32_t start;
};

static int part_read(struct blockdev *d, uint32_t lba, uint32_t count, void *buf)
{
    struct part *p = d->priv;
    return blockdev_read(p->parent, p->start + lba, count, buf);
}

static int part_write(struct blockdev *d, uint32_t lba, uint32_t count, const void *buf)
{
    struct part *p = d->priv;
    return blockdev_write(p->parent, p->start + lba, count, buf);
}

int partition_scan(struct blockdev *disk)
{
    uint8_t sec[SECTOR_SIZE];
    int found = 0;

    if (strcmp(disk->kind, "partition") == 0 || disk->sector_count < 2)
        return 0;
    if (blockdev_read(disk, 0, 1, sec) < 0)
        return 0;
    if (sec[510] != 0x55 || sec[511] != 0xAA)
        return 0;

    const struct mbr_entry *e = (const struct mbr_entry *)(sec + 446);
    for (int i = 0; i < 4; i++) {
        if (e[i].type == 0 || e[i].sectors == 0)
            continue;
        if (e[i].lba_start >= disk->sector_count ||
            e[i].sectors > disk->sector_count - e[i].lba_start)
            continue;                                   /* bogus entry */

        struct part *p = kzalloc(sizeof *p);
        struct blockdev *d = kzalloc(sizeof *d);
        if (!p || !d) {
            kfree(p);
            kfree(d);
            return found;
        }
        p->parent = disk;
        p->start = e[i].lba_start;
        strlcpy(d->name, disk->name, sizeof d->name);
        size_t n = strlen(d->name);
        d->name[n] = (char)('1' + i);
        d->name[n + 1] = '\0';
        d->kind = "partition";
        d->sector_count = e[i].sectors;
        d->read = part_read;
        d->write = part_write;
        d->priv = p;
        if (blockdev_register(d) < 0) {
            kfree(p);
            kfree(d);
        } else {
            found++;
        }
    }
    return found;
}

int partition_write_mbr(struct blockdev *d, uint32_t start, uint32_t count, uint8_t type)
{
    uint8_t sec[SECTOR_SIZE];

    if (start == 0 || start >= d->sector_count ||
        count == 0 || count > d->sector_count - start)
        return -EINVAL;
    memset(sec, 0, sizeof sec);
    struct mbr_entry *e = (struct mbr_entry *)(sec + 446);
    e[0].status = 0x00;
    e[0].type = type;
    e[0].lba_start = start;
    e[0].sectors = count;
    sec[510] = 0x55;
    sec[511] = 0xAA;
    return blockdev_write(d, 0, 1, sec);
}
