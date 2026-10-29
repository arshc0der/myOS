/*
 * ramdisk.c - a disk that lives in RAM. Fast, needs no hardware, and is
 * erased at power-off. Great for experimenting with your own filesystem.
 */
#include "blockdev.h"
#include "pmm.h"
#include "heap.h"
#include "kstring.h"
#include "kerror.h"

struct ramdisk {
    uint8_t *mem;
    bool     owned;                 /* we allocated it with the PMM */
    uint32_t pages;
};

static int ram_read(struct blockdev *d, uint32_t lba, uint32_t count, void *buf)
{
    struct ramdisk *r = d->priv;
    memcpy(buf, r->mem + (size_t)lba * SECTOR_SIZE, (size_t)count * SECTOR_SIZE);
    return 0;
}

static int ram_write(struct blockdev *d, uint32_t lba, uint32_t count, const void *buf)
{
    struct ramdisk *r = d->priv;
    memcpy(r->mem + (size_t)lba * SECTOR_SIZE, buf, (size_t)count * SECTOR_SIZE);
    return 0;
}

static int make(const char *name, uint8_t *mem, uint32_t size, bool owned,
                uint32_t pages)
{
    struct ramdisk *r = kzalloc(sizeof *r);
    struct blockdev *d = kzalloc(sizeof *d);

    if (!r || !d) {
        kfree(r);
        kfree(d);
        return -ENOMEM;
    }
    r->mem = mem;
    r->owned = owned;
    r->pages = pages;
    strlcpy(d->name, name, sizeof d->name);
    d->kind = "ram";
    d->sector_count = size / SECTOR_SIZE;
    d->read = ram_read;
    d->write = ram_write;
    d->priv = r;
    int rc = blockdev_register(d);
    if (rc < 0) {
        if (owned)
            pmm_free_pages((paddr_t)mem, pages);
        kfree(r);
        kfree(d);
    }
    return rc;
}

int ramdisk_create(const char *name, uint32_t size_bytes)
{
    uint32_t pages = (size_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    paddr_t mem;

    if (pages == 0)
        return -EINVAL;
    mem = pmm_alloc_contiguous(pages);
    if (!mem)
        return -ENOMEM;
    memset((void *)mem, 0, (size_t)pages * PAGE_SIZE);
    return make(name, (uint8_t *)mem, pages * PAGE_SIZE, true, pages);
}

int ramdisk_create_from_memory(const char *name, void *mem, uint32_t size_bytes)
{
    return make(name, mem, size_bytes & ~(SECTOR_SIZE - 1u), false, 0);
}

int ramdisk_destroy(const char *name)
{
    struct blockdev *d = blockdev_find(name);

    if (!d || strcmp(d->kind, "ram") != 0)
        return -ENODEV;
    struct ramdisk *r = d->priv;
    blockdev_unregister(d);
    if (r->owned)
        pmm_free_pages((paddr_t)r->mem, r->pages);
    kfree(r);
    kfree(d);
    return 0;
}
