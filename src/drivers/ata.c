/*
 * ata.c - IDE/ATA hard disk driver using PIO ("programmed I/O").
 *
 * The CPU talks to the disk through I/O ports and copies each sector word
 * by word. It is slow compared with DMA, but simple and works everywhere,
 * including QEMU/Bochs/VirtualBox "IDE" disks.
 *
 * Note: virtual machines with a modern chipset (for example the QEMU
 * "q35" machine with SATA/AHCI drives) do not provide legacy IDE ports.
 * Use an IDE drive, or use a RAM disk.
 */
#include "blockdev.h"
#include "io.h"
#include "heap.h"
#include "kstring.h"
#include "kerror.h"
#include "sync.h"
#include "log.h"

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF  0x20
#define ST_RDY 0x40
#define ST_BSY 0x80

struct ata_drive {
    uint16_t io, ctl;
    uint8_t  slave;
};

static struct mutex ata_lock = MUTEX_INIT(ata_lock);

static void delay400(uint16_t ctl)
{
    for (int i = 0; i < 4; i++)
        inb(ctl);
}

static int wait_not_busy(uint16_t io, uint16_t ctl)
{
    delay400(ctl);
    for (int i = 0; i < 4000000; i++) {
        uint8_t s = inb(io + 7);
        if (!(s & ST_BSY))
            return 0;
    }
    return -ETIMEDOUT;
}

static int wait_drq(uint16_t io, uint16_t ctl)
{
    delay400(ctl);
    for (int i = 0; i < 4000000; i++) {
        uint8_t s = inb(io + 7);
        if (s & (ST_ERR | ST_DF))
            return -EIO;
        if (!(s & ST_BSY) && (s & ST_DRQ))
            return 0;
    }
    return -ETIMEDOUT;
}

static void select_lba(struct ata_drive *a, uint32_t lba, uint32_t count)
{
    outb(a->io + 6, 0xE0 | (a->slave << 4) | ((lba >> 24) & 0x0F));
    outb(a->io + 2, (uint8_t)count);
    outb(a->io + 3, (uint8_t)lba);
    outb(a->io + 4, (uint8_t)(lba >> 8));
    outb(a->io + 5, (uint8_t)(lba >> 16));
}

static int ata_read(struct blockdev *d, uint32_t lba, uint32_t count, void *buf)
{
    struct ata_drive *a = d->priv;
    uint8_t *out = buf;
    int rc = 0;

    mutex_lock(&ata_lock);
    while (count && rc == 0) {
        uint32_t n = count > 128 ? 128 : count;
        if ((rc = wait_not_busy(a->io, a->ctl)) < 0)
            break;
        select_lba(a, lba, n);
        outb(a->io + 7, 0x20);                  /* READ SECTORS */
        for (uint32_t i = 0; i < n; i++) {
            if ((rc = wait_drq(a->io, a->ctl)) < 0)
                break;
            insw(a->io, out, 256);
            out += SECTOR_SIZE;
        }
        lba += n;
        count -= n;
    }
    mutex_unlock(&ata_lock);
    return rc;
}

static int ata_write(struct blockdev *d, uint32_t lba, uint32_t count, const void *buf)
{
    struct ata_drive *a = d->priv;
    const uint8_t *in = buf;
    int rc = 0;

    mutex_lock(&ata_lock);
    while (count && rc == 0) {
        uint32_t n = count > 128 ? 128 : count;
        if ((rc = wait_not_busy(a->io, a->ctl)) < 0)
            break;
        select_lba(a, lba, n);
        outb(a->io + 7, 0x30);                  /* WRITE SECTORS */
        for (uint32_t i = 0; i < n; i++) {
            if ((rc = wait_drq(a->io, a->ctl)) < 0)
                break;
            outsw(a->io, in, 256);
            in += SECTOR_SIZE;
        }
        if (rc == 0) {
            outb(a->io + 7, 0xE7);              /* FLUSH CACHE */
            rc = wait_not_busy(a->io, a->ctl);
        }
        lba += n;
        count -= n;
    }
    mutex_unlock(&ata_lock);
    return rc;
}

static bool probe(uint16_t io, uint16_t ctl, int slave, int index)
{
    uint16_t id[256];

    outb(io + 6, 0xA0 | (slave << 4));
    delay400(ctl);
    outb(io + 2, 0); outb(io + 3, 0); outb(io + 4, 0); outb(io + 5, 0);
    outb(io + 7, 0xEC);                         /* IDENTIFY */
    uint8_t st = inb(io + 7);
    if (st == 0 || st == 0xFF)
        return false;                           /* nothing there */
    for (int i = 0; i < 1000000 && (inb(io + 7) & ST_BSY); i++)
        ;
    if (inb(io + 4) || inb(io + 5))
        return false;                           /* ATAPI (CD-ROM) etc. */
    for (int i = 0; i < 1000000; i++) {
        st = inb(io + 7);
        if (st & (ST_ERR | ST_DF))
            return false;
        if (st & ST_DRQ)
            break;
    }
    if (!(st & ST_DRQ))
        return false;
    insw(io, id, 256);

    uint32_t sectors = id[60] | ((uint32_t)id[61] << 16);
    if (sectors == 0)
        return false;

    struct ata_drive *a = kzalloc(sizeof *a);
    struct blockdev *d = kzalloc(sizeof *d);
    if (!a || !d) {
        kfree(a);
        kfree(d);
        return false;
    }
    a->io = io;
    a->ctl = ctl;
    a->slave = (uint8_t)slave;
    d->name[0] = 'h'; d->name[1] = 'd'; d->name[2] = (char)('a' + index);
    d->kind = "ata";
    d->sector_count = sectors;
    d->read = ata_read;
    d->write = ata_write;
    d->priv = a;
    for (int i = 0; i < 20; i++) {              /* model: byte-swapped words */
        d->model[i * 2] = (char)(id[27 + i] >> 8);
        d->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    d->model[40] = '\0';
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--)
        d->model[i] = '\0';
    if (blockdev_register(d) < 0) {
        kfree(a);
        kfree(d);
        return false;
    }
    partition_scan(d);
    return true;
}

void ata_init(void)
{
    static const struct { uint16_t io, ctl; } bus[2] = {
        { 0x1F0, 0x3F6 }, { 0x170, 0x376 }
    };
    int index = 0, found = 0;

    for (int b = 0; b < 2; b++) {
        if (inb(bus[b].io + 7) == 0xFF) {           /* floating bus */
            index += 2;
            continue;
        }
        for (int s = 0; s < 2; s++, index++)
            if (probe(bus[b].io, bus[b].ctl, s, index))
                found++;
    }
    if (found == 0)
        log_info("ata", "no IDE disks found (use a RAM disk, or an IDE drive in your VM)");
}
