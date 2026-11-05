/*
 * virtio_net.c - virtio network card (legacy I/O interface), the card UTM and QEMU offer as
 * "virtio-net-pci". Polling only: queue 0 receives, queue 1 transmits.
 */
#include "net.h"
#include "hw.h"
#include "io.h"
#include "pmm.h"
#include "timer.h"
#include "log.h"
#include "kstring.h"

#define VIO_FEAT   0x00
#define VIO_GFEAT  0x04
#define VIO_QPFN   0x08
#define VIO_QSIZE  0x0C
#define VIO_QSEL   0x0E
#define VIO_QNOTE  0x10
#define VIO_STATUS 0x12
#define VIO_CFG    0x14
#define HDR 10                                  /* struct virtio_net_hdr without mergeable buffers */
#define NBUF 32

struct vq {
    uint16_t size;
    uint8_t *desc;                              /* 16 bytes each */
    volatile uint16_t *avail;                   /* flags, idx, ring[] */
    volatile uint8_t *used;                     /* flags, idx, {id,len}[] */
    uint16_t last_used;
    uint16_t qidx;
};

static uint16_t io;
static struct vq rxq, txq;
static uint8_t *rxbuf, *txbuf;
static struct netdev nic;

static int setup_queue(struct vq *q, uint16_t idx)
{
    outw(io + VIO_QSEL, idx);
    uint16_t n = inw(io + VIO_QSIZE);
    if (!n)
        return -1;
    size_t desc = 16u * n, avail = 6u + 2u * n;
    size_t used_off = (desc + avail + 4095) & ~(size_t)4095;
    size_t total = used_off + 6u + 8u * n;
    uint32_t pages = (uint32_t)((total + 4095) / 4096);
    paddr_t mem = pmm_alloc_contiguous(pages);
    if (!mem)
        return -1;
    memset((void *)mem, 0, (size_t)pages * 4096);
    q->size = n;
    q->qidx = idx;
    q->desc = (uint8_t *)mem;
    q->avail = (volatile uint16_t *)(mem + desc);
    q->used = (volatile uint8_t *)(mem + used_off);
    q->last_used = 0;
    outl(io + VIO_QPFN, (uint32_t)(mem >> 12));
    return 0;
}

static void set_desc(struct vq *q, int i, uint32_t addr, uint32_t len, uint16_t flags)
{
    uint8_t *d = q->desc + i * 16;
    *(uint64_t *)d = addr;
    *(uint32_t *)(d + 8) = len;
    *(uint16_t *)(d + 12) = flags;
    *(uint16_t *)(d + 14) = 0;
}

static void push_avail(struct vq *q, uint16_t id)
{
    uint16_t idx = q->avail[1];
    q->avail[2 + (idx % q->size)] = id;
    __asm__ volatile("" ::: "memory");
    q->avail[1] = (uint16_t)(idx + 1);
    outw(io + VIO_QNOTE, q->qidx);
}

static int vio_send(struct netdev *d, const void *frame, size_t len)
{
    (void)d;
    if (len > 1514)
        return -1;
    memset(txbuf, 0, HDR);
    memcpy(txbuf + HDR, frame, len);
    set_desc(&txq, 0, (uint32_t)txbuf, (uint32_t)(HDR + len), 0);
    uint16_t before = *(volatile uint16_t *)(txq.used + 2);
    push_avail(&txq, 0);
    for (int i = 0; i < 200; i++) {
        if (*(volatile uint16_t *)(txq.used + 2) != before)
            return 0;
        timer_sleep_ms(1);
    }
    return -1;
}

static int vio_poll(struct netdev *d, void *buf, size_t max)
{
    (void)d;
    uint16_t used_idx = *(volatile uint16_t *)(rxq.used + 2);
    if (used_idx == rxq.last_used)
        return 0;
    volatile uint8_t *e = rxq.used + 4 + 8 * (rxq.last_used % rxq.size);
    uint32_t id = *(volatile uint32_t *)e, len = *(volatile uint32_t *)(e + 4);
    rxq.last_used++;
    int out = 0;
    if (len > HDR && id < NBUF) {
        size_t n = len - HDR;
        if (n > max)
            n = max;
        memcpy(buf, rxbuf + id * 2048 + HDR, n);
        out = (int)n;
    }
    if (id < NBUF)
        push_avail(&rxq, (uint16_t)id);
    return out;
}

int virtio_net_probe(void)
{
    struct pci_device list[64];
    int n = pci_scan(list, 64);

    for (int i = 0; i < n; i++) {
        struct pci_device *pd = &list[i];
        if (pd->vendor != 0x1AF4 || pd->device != 0x1000)       /* legacy / transitional virtio-net */
            continue;
        uint32_t bar = pci_cfg_read(pd->bus, pd->slot, pd->func, 0x10);
        if (!(bar & 1)) {
            log_warn("net", "virtio-net: BAR0 is not an I/O range");
            continue;
        }
        uint32_t cmd = pci_cfg_read(pd->bus, pd->slot, pd->func, 4);
        pci_cfg_write(pd->bus, pd->slot, pd->func, 4, (cmd & 0xFFFF) | 0x7);
        io = (uint16_t)(bar & ~3u);
        outb(io + VIO_STATUS, 0);                               /* reset */
        outb(io + VIO_STATUS, 1);                               /* acknowledge */
        outb(io + VIO_STATUS, 1 | 2);                           /* driver */
        uint32_t feat = inl(io + VIO_FEAT);
        outl(io + VIO_GFEAT, feat & (1u << 5));                 /* only: the card has a MAC address */
        if (setup_queue(&rxq, 0) < 0 || setup_queue(&txq, 1) < 0) {
            log_warn("net", "virtio-net: queue setup failed");
            outb(io + VIO_STATUS, 0x80);
            continue;
        }
        paddr_t mem = pmm_alloc_contiguous(18);                 /* 32 x 2 KiB receive + one transmit buffer */
        if (!mem) {
            log_warn("net", "virtio-net: out of memory");
            return 0;
        }
        memset((void *)mem, 0, 18 * 4096);
        rxbuf = (uint8_t *)mem;
        txbuf = (uint8_t *)(mem + NBUF * 2048);
        for (int k = 0; k < NBUF; k++) {
            set_desc(&rxq, k, (uint32_t)(rxbuf + k * 2048), 2048, 2);   /* 2 = device writes */
            rxq.avail[2 + k] = (uint16_t)k;
        }
        rxq.avail[1] = NBUF;
        outb(io + VIO_STATUS, 1 | 2 | 4);                       /* driver ok */
        outw(io + VIO_QNOTE, 0);
        for (int k = 0; k < 6; k++)
            nic.mac[k] = inb((uint16_t)(io + VIO_CFG + k));
        nic.driver = "virtio-net";
        nic.send = vio_send;
        nic.poll = vio_poll;
        nic.link = true;
        net_register(&nic);
        log_info("net", "virtio-net %02x:%02x:%02x:%02x:%02x:%02x", nic.mac[0], nic.mac[1], nic.mac[2], nic.mac[3],
                 nic.mac[4], nic.mac[5]);
        return 1;
    }
    return 0;
}
