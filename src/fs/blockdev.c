/* blockdev.c - registry of block devices and checked read/write wrappers. */
#include "blockdev.h"
#include "heap.h"
#include "kstring.h"
#include "kerror.h"
#include "log.h"

static LIST_HEAD(devices);

int blockdev_register(struct blockdev *d)
{
    if (blockdev_find(d->name))
        return -EEXIST;
    list_add_tail(&d->node, &devices);
    log_info("block", "%s: %s, %u sectors (%u KiB)%s%s%s", d->name, d->kind,
             d->sector_count, d->sector_count / 2,
             d->model[0] ? " " : "", d->model, d->readonly ? " [read-only]" : "");
    return 0;
}

void blockdev_unregister(struct blockdev *d)
{
    list_del(&d->node);
}

struct blockdev *blockdev_find(const char *name)
{
    struct list_head *p;

    list_for_each(p, &devices) {
        struct blockdev *d = list_entry(p, struct blockdev, node);
        if (strcmp(d->name, name) == 0)
            return d;
    }
    return NULL;
}

void blockdev_for_each(void (*cb)(struct blockdev *, void *), void *ctx)
{
    struct list_head *p, *n;

    list_for_each_safe(p, n, &devices)
        cb(list_entry(p, struct blockdev, node), ctx);
}

int blockdev_count(void)
{
    int n = 0;
    struct list_head *p;

    list_for_each(p, &devices)
        n++;
    return n;
}

int blockdev_read(struct blockdev *d, uint32_t lba, uint32_t count, void *buf)
{
    if (!d || !d->read)
        return -ENODEV;
    if (count == 0)
        return 0;
    if (lba >= d->sector_count || count > d->sector_count - lba)
        return -ERANGE;
    return d->read(d, lba, count, buf);
}

int blockdev_write(struct blockdev *d, uint32_t lba, uint32_t count, const void *buf)
{
    if (!d || !d->write)
        return -ENODEV;
    if (d->readonly)
        return -EPERM;
    if (count == 0)
        return 0;
    if (lba >= d->sector_count || count > d->sector_count - lba)
        return -ERANGE;
    return d->write(d, lba, count, buf);
}

int blockdev_read_bytes(struct blockdev *d, uint64_t off, void *buf, uint32_t len)
{
    uint8_t sec[SECTOR_SIZE];
    uint8_t *out = buf;

    if (off + len > blockdev_size(d))
        return -ERANGE;
    while (len) {
        uint32_t lba = (uint32_t)(off / SECTOR_SIZE);
        uint32_t in = (uint32_t)(off % SECTOR_SIZE);
        uint32_t n = MIN(len, SECTOR_SIZE - in);
        int r = blockdev_read(d, lba, 1, sec);
        if (r < 0)
            return r;
        memcpy(out, sec + in, n);
        out += n;
        off += n;
        len -= n;
    }
    return 0;
}

int blockdev_write_bytes(struct blockdev *d, uint64_t off, const void *buf, uint32_t len)
{
    uint8_t sec[SECTOR_SIZE];
    const uint8_t *in = buf;

    if (off + len > blockdev_size(d))
        return -ERANGE;
    while (len) {
        uint32_t lba = (uint32_t)(off / SECTOR_SIZE);
        uint32_t at = (uint32_t)(off % SECTOR_SIZE);
        uint32_t n = MIN(len, SECTOR_SIZE - at);
        int r;
        if (n != SECTOR_SIZE) {                 /* partial: read-modify-write */
            r = blockdev_read(d, lba, 1, sec);
            if (r < 0)
                return r;
        }
        memcpy(sec + at, in, n);
        r = blockdev_write(d, lba, 1, sec);
        if (r < 0)
            return r;
        in += n;
        off += n;
        len -= n;
    }
    return 0;
}
