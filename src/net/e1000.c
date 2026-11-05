/*
 * e1000.c - Intel 82540EM ("e1000"), the network card QEMU emulates by default.
 * Polling only (no interrupts): 32 receive buffers, 8 transmit descriptors.
 */
#include "net.h"
#include "hw.h"
#include "vmm.h"
#include "pmm.h"
#include "timer.h"
#include "log.h"
#include "kstring.h"

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404

#define NRX 32
#define NTX 8

struct rxd { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; } __attribute__((packed));
struct txd { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } __attribute__((packed));

static volatile uint8_t *mmio;
static struct rxd *rx;
static struct txd *tx;
static uint8_t *rxbuf, *txbuf;
static int rx_cur, tx_tail;
static struct netdev nic;

static uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }
static void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }

static int e1000_send(struct netdev *d, const void *frame, size_t len)
{
    (void)d;
    if (len > 1518)
        return -1;
    struct txd *t = &tx[tx_tail];
    for (int i = 0; i < 200 && tx_tail != 0 && !(t->status & 1) && t->cmd; i++)   /* wait for a free slot */
        timer_sleep_ms(1);
    memcpy(txbuf + tx_tail * 2048, frame, len);
    t->addr = (uint64_t)(uint32_t)(txbuf + tx_tail * 2048);
    t->len = (uint16_t)len;
    t->cmd = 0x0B;                                  /* end of packet, insert CRC, report status */
    t->status = 0;
    tx_tail = (tx_tail + 1) % NTX;
    wr(REG_TDT, (uint32_t)tx_tail);
    for (int i = 0; i < 100; i++) {
        if (t->status & 1)
            return 0;
        timer_sleep_ms(1);
    }
    return -1;
}

static int e1000_poll(struct netdev *d, void *buf, size_t max)
{
    struct rxd *r = &rx[rx_cur];
    (void)d;
    if (!(r->status & 1))
        return 0;
    size_t len = r->len;
    if (len > max)
        len = max;
    memcpy(buf, rxbuf + rx_cur * 2048, len);
    r->status = 0;
    wr(REG_RDT, (uint32_t)rx_cur);
    rx_cur = (rx_cur + 1) % NRX;
    nic.link = (rd(REG_STATUS) & 2) != 0;
    return (int)len;
}

static uint16_t eeprom(int word)
{
    wr(REG_EERD, ((uint32_t)word << 8) | 1);
    for (int i = 0; i < 1000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & 0x10)
            return (uint16_t)(v >> 16);
    }
    return 0;
}

int e1000_probe(void)
{
    struct pci_device list[64];
    int n = pci_scan(list, 64);

    for (int i = 0; i < n; i++) {
        struct pci_device *pd = &list[i];
        if (pd->vendor != 0x8086 || (pd->device != 0x100E && pd->device != 0x100F && pd->device != 0x1004 &&
                                     pd->device != 0x100C && pd->device != 0x10D3 && pd->device != 0x153A))
            continue;
        uint32_t cmd = pci_cfg_read(pd->bus, pd->slot, pd->func, 4);
        pci_cfg_write(pd->bus, pd->slot, pd->func, 4, (cmd & 0xFFFF) | 0x7);     /* I/O, memory, bus master */
        uint32_t bar = pci_cfg_read(pd->bus, pd->slot, pd->func, 0x10);
        if ((bar & 1) || !(bar & ~0xFu)) {
            log_warn("net", "e1000: no memory BAR");
            continue;
        }
        uint32_t base = bar & ~0xFu;
        if (vmm_map_range(base, base, 0x20000, VMM_WRITE | VMM_NOCACHE) < 0) {
            log_warn("net", "e1000: cannot map registers");
            continue;
        }
        mmio = (volatile uint8_t *)base;
        paddr_t mem = pmm_alloc_contiguous(24);                  /* rings + buffers, 96 KiB */
        if (!mem) {
            log_warn("net", "e1000: out of memory");
            return 0;
        }
        memset((void *)mem, 0, 24 * 4096);
        rx = (struct rxd *)mem;                                  /* 512 bytes */
        tx = (struct txd *)(mem + 1024);                         /* 128 bytes */
        rxbuf = (uint8_t *)(mem + 4096);                         /* 64 KiB */
        txbuf = (uint8_t *)(mem + 4096 + NRX * 2048);            /* 16 KiB */

        wr(REG_CTRL, rd(REG_CTRL) | (1u << 26));                 /* reset */
        timer_sleep_ms(20);
        wr(REG_IMC, 0xFFFFFFFFu);                                /* no interrupts */
        wr(REG_CTRL, (rd(REG_CTRL) | (1u << 6) | (1u << 5)) & ~(1u << 3));   /* set link up, auto speed */

        uint32_t ral = rd(REG_RAL0), rah = rd(REG_RAH0);
        if (!ral && !(rah & 0xFFFF)) {
            uint16_t w0 = eeprom(0), w1 = eeprom(1), w2 = eeprom(2);
            ral = w0 | ((uint32_t)w1 << 16);
            rah = w2 | (1u << 31);
            wr(REG_RAL0, ral);
            wr(REG_RAH0, rah);
        }
        nic.mac[0] = (uint8_t)ral; nic.mac[1] = (uint8_t)(ral >> 8);
        nic.mac[2] = (uint8_t)(ral >> 16); nic.mac[3] = (uint8_t)(ral >> 24);
        nic.mac[4] = (uint8_t)rah; nic.mac[5] = (uint8_t)(rah >> 8);
        for (int k = 0; k < 128; k++)
            wr(REG_MTA + (uint32_t)k * 4, 0);

        for (int k = 0; k < NRX; k++)
            rx[k].addr = (uint64_t)(uint32_t)(rxbuf + k * 2048);
        wr(REG_RDBAL, (uint32_t)rx);
        wr(REG_RDBAH, 0);
        wr(REG_RDLEN, NRX * sizeof(struct rxd));
        wr(REG_RDH, 0);
        wr(REG_RDT, NRX - 1);
        wr(REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26));       /* enable, accept broadcast, strip CRC, 2 KiB buffers */
        for (int k = 0; k < NTX; k++)
            tx[k].status = 1;
        wr(REG_TDBAL, (uint32_t)tx);
        wr(REG_TDBAH, 0);
        wr(REG_TDLEN, NTX * sizeof(struct txd));
        wr(REG_TDH, 0);
        wr(REG_TDT, 0);
        wr(REG_TIPG, 0x0060200A);
        wr(REG_TCTL, (1u << 1) | (1u << 3) | (0x0Fu << 4) | (0x40u << 12));

        nic.driver = "e1000";
        nic.send = e1000_send;
        nic.poll = e1000_poll;
        nic.link = (rd(REG_STATUS) & 2) != 0;
        net_register(&nic);
        log_info("net", "e1000 %02x:%02x:%02x:%02x:%02x:%02x link %s", nic.mac[0], nic.mac[1], nic.mac[2], nic.mac[3],
                 nic.mac[4], nic.mac[5], nic.link ? "up" : "down");
        return 1;
    }
    return 0;
}
