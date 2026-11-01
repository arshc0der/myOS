/*
 * xhci.c - USB 3 (xHCI) host controller driver.
 *
 * How it works, in short:
 *   - The controller and the driver talk through "rings" in RAM: a command
 *     ring (driver -> controller), one transfer ring per endpoint, and one
 *     event ring (controller -> driver). Every entry is a 16-byte TRB.
 *   - A "cycle bit" tells each side whether an entry is new.
 *   - We never use interrupts: the "usb" task (usbcore.c) polls the event ring.
 *   - Each connected device gets a "slot". usbcore.c reads the descriptors,
 *     handles hubs and picks the HID interfaces; this file only provides the
 *     controller-specific part through struct usb_hc_ops.
 *
 * Devices behind hubs need a route string and, for low/full speed devices
 * behind a high speed hub, the hub's slot + port (the "transaction
 * translator") in the slot context.
 */
#include "usbcore.h"
#include "io.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "timer.h"
#include "kstring.h"
#include "kprintf.h"
#include "log.h"

/* ---- TRB types ---- */
#define TRB_NORMAL   1
#define TRB_SETUP    2
#define TRB_DATA     3
#define TRB_STATUS   4
#define TRB_LINK     6
#define CMD_ENABLE_SLOT   9
#define CMD_DISABLE_SLOT  10
#define CMD_ADDRESS_DEV   11
#define CMD_CONFIG_EP     12
#define CMD_EVAL_CTX      13
#define CMD_RESET_EP      14
#define CMD_SET_TR_DEQ    16
#define EV_TRANSFER  32
#define EV_CMD_DONE  33
#define EV_PORT_CHG  34

#define CC_SUCCESS 1
#define CC_SHORT   13

#define RING_TRBS  64

struct trb { uint32_t p0, p1, status, ctrl; };

struct ring {
    volatile struct trb *t;
    paddr_t phys;
    int n, enq, pcs;
};

struct xep {
    int dci, mps;
    struct ring ring;
    int errors;
    struct usb_ep *ue;
};

struct xdev {
    bool used;
    int slot;
    struct usb_dev *ud;
    paddr_t out_p, in_p;
    uint8_t *out, *in;                  /* device ctx, input ctx */
    struct ring ep0;
    struct xep xe[USB_MAX_EPS];
};

static struct {
    bool ok;
    volatile uint8_t *cap, *op, *rt, *db;
    int ctxsz, maxslots, maxports;
    uint32_t *dcbaa;
    struct ring cmd;
    volatile struct trb *evt;
    paddr_t evt_phys;
    int evt_idx, evt_ccs;
    uint32_t pagesize;
} X;

static struct xdev xdevs[USB_MAX_DEVS];

/* results of the command / control transfer we are waiting for */
static struct { volatile bool done; uint32_t code, slot, p0; } cmd_res;
static struct { volatile bool done; uint32_t code, slot; } ctl_res;

static inline uint32_t rd(volatile void *p) { return *(volatile uint32_t *)p; }
static inline void wr(volatile void *p, uint32_t v) { *(volatile uint32_t *)p = v; }
static inline void wr64(volatile void *p, paddr_t v) { wr(p, v); wr((volatile uint8_t *)p + 4, 0); }
static inline void barrier(void) { __asm__ volatile("" ::: "memory"); }

static void *dma_page(paddr_t *phys) { return usb_dma_pages(1, phys); }
static void dma_free(void *p) { usb_dma_free(p, 1); }

static bool ring_alloc(struct ring *r, int n)
{
    paddr_t ph;

    r->t = dma_page(&ph);
    if (!r->t)
        return false;
    r->phys = ph;
    r->n = n;
    r->enq = 0;
    r->pcs = 1;
    volatile struct trb *l = &r->t[n - 1];             /* link back to the start */
    l->p0 = ph;
    l->p1 = 0;
    l->status = 0;
    l->ctrl = (TRB_LINK << 10) | (1 << 1);             /* toggle cycle */
    return true;
}

/* Add a TRB; returns its physical address. ctrl must not contain the cycle bit. */
static paddr_t ring_push(struct ring *r, uint32_t p0, uint32_t p1, uint32_t status, uint32_t ctrl)
{
    volatile struct trb *t = &r->t[r->enq];
    paddr_t at = r->phys + (paddr_t)r->enq * 16;

    t->p0 = p0;
    t->p1 = p1;
    t->status = status;
    barrier();
    t->ctrl = ctrl | (uint32_t)r->pcs;
    barrier();
    if (++r->enq == r->n - 1) {
        r->t[r->n - 1].ctrl = (r->t[r->n - 1].ctrl & ~1u) | (uint32_t)r->pcs;
        r->enq = 0;
        r->pcs ^= 1;
    }
    return at;
}

static void ring_doorbell(int slot, int target) { wr(X.db + 4 * slot, (uint32_t)target); }

static void ring_free(struct ring *r)
{
    dma_free((void *)r->t);
    r->t = NULL;
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static struct xdev *find_dev(int slot)
{
    for (int i = 0; i < USB_MAX_DEVS; i++)
        if (xdevs[i].used && xdevs[i].slot == slot)
            return &xdevs[i];
    return NULL;
}

static void requeue(struct xdev *d, struct xep *e)
{
    ring_push(&e->ring, (uint32_t)(paddr_t)e->ue->buf, 0, (uint32_t)e->mps,
              (TRB_NORMAL << 10) | (1 << 5) | (1 << 2));
    ring_doorbell(d->slot, e->dci);
}

static void handle_event(volatile struct trb *t)
{
    uint32_t ctrl = t->ctrl, status = t->status;
    int type = (int)((ctrl >> 10) & 63);
    uint32_t code = status >> 24;

    if (type == EV_CMD_DONE) {
        cmd_res.code = code;
        cmd_res.slot = ctrl >> 24;
        cmd_res.p0 = t->p0;
        cmd_res.done = true;
    } else if (type == EV_TRANSFER) {
        int slot = (int)(ctrl >> 24), dci = (int)((ctrl >> 16) & 31);
        if (dci == 1) {
            ctl_res.code = code;
            ctl_res.slot = (uint32_t)slot;
            ctl_res.done = true;
            return;
        }
        struct xdev *d = find_dev(slot);
        if (!d)
            return;
        for (int i = 0; i < USB_MAX_EPS; i++) {
            struct xep *e = &d->xe[i];
            if (!e->ue || e->dci != dci)
                continue;
            if (code == CC_SUCCESS || code == CC_SHORT) {
                int got = e->mps - (int)(status & 0xFFFFFF);
                if (got > 0)
                    usb_report(d->ud, e->ue, e->ue->buf, got);
                e->errors = 0;
                requeue(d, e);
            } else if (++e->errors < 8) {
                log_warn("usb", "slot %d endpoint %d: transfer error %u", slot, dci, code);
                requeue(d, e);
            }
            return;
        }
    }
}

static void events_poll(void)
{
    bool any = false;

    for (;;) {
        volatile struct trb *t = &X.evt[X.evt_idx];
        if ((int)(rd(&t->ctrl) & 1) != X.evt_ccs)
            break;
        barrier();
        handle_event(t);
        any = true;
        if (++X.evt_idx == 256) {
            X.evt_idx = 0;
            X.evt_ccs ^= 1;
        }
    }
    if (any) {
        volatile uint8_t *ir = X.rt + 0x20;
        paddr_t erdp = X.evt_phys + (paddr_t)X.evt_idx * 16;
        wr(ir + 0x18, erdp | 8);                        /* bit 3 clears "event handler busy" */
        wr(ir + 0x1C, 0);
    }
}

static void xhci_poll(struct usb_hc *hc)
{
    (void)hc;
    events_poll();
}

/* Wait until *flag becomes true (events are processed meanwhile). */
static bool wait_flag(volatile bool *flag, uint32_t timeout_ticks)
{
    uint32_t start = timer_ticks();

    for (int spin = 0;; spin++) {
        events_poll();
        if (*flag)
            return true;
        if ((uint32_t)(timer_ticks() - start) > timeout_ticks)
            return false;
        if (spin > 2000)
            task_yield();
    }
}

/* ------------------------------------------------------------------ */
/* Commands and control transfers                                      */
/* ------------------------------------------------------------------ */

static int command(uint32_t p0, uint32_t ctrl, uint32_t *slot_out)
{
    cmd_res.done = false;
    ring_push(&X.cmd, p0, 0, 0, ctrl);
    ring_doorbell(0, 0);
    if (!wait_flag(&cmd_res.done, 200))
        return -1;
    if (slot_out)
        *slot_out = cmd_res.slot;
    return (int)cmd_res.code;
}

static void ep0_recover(struct xdev *d)
{
    command(0, (CMD_RESET_EP << 10) | ((uint32_t)d->slot << 24) | (1u << 16), NULL);
    command(d->ep0.phys + (uint32_t)d->ep0.enq * 16 + (uint32_t)d->ep0.pcs,
            (CMD_SET_TR_DEQ << 10) | ((uint32_t)d->slot << 24) | (1u << 16), NULL);
}

static int xhci_ctrl(struct usb_dev *ud, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len)
{
    struct xdev *d = ud->hcpriv;
    bool in = (rt & 0x80) != 0;
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;

    ctl_res.done = false;
    ring_push(&d->ep0, (uint32_t)rt | ((uint32_t)req << 8) | ((uint32_t)val << 16),
              (uint32_t)idx | ((uint32_t)len << 16), 8, (TRB_SETUP << 10) | (1 << 6) | (trt << 16));
    if (len)
        ring_push(&d->ep0, (uint32_t)(paddr_t)ud->buf, 0, len, (TRB_DATA << 10) | (in ? (1u << 16) : 0));
    ring_push(&d->ep0, 0, 0, 0, (TRB_STATUS << 10) | (1 << 5) | ((len && in) ? 0u : (1u << 16)));
    ring_doorbell(d->slot, 1);
    if (!wait_flag(&ctl_res.done, 300)) {
        log_warn("usb", "slot %d: control transfer timed out", d->slot);
        return -1;
    }
    if (ctl_res.code != CC_SUCCESS && ctl_res.code != CC_SHORT) {
        if (ctl_res.code == 6)
            ep0_recover(d);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Contexts                                                            */
/* ------------------------------------------------------------------ */

static uint32_t *ctx(uint8_t *base, int index) { return (uint32_t *)(base + (size_t)index * (size_t)X.ctxsz); }

static int ilog2(uint32_t v)
{
    int r = 0;

    while (v > 1) {
        v >>= 1;
        r++;
    }
    return r;
}

static int xspeed(int s)            /* usbcore speed -> xHCI speed id */
{
    switch (s) {
    case USB_FULL: return 1;
    case USB_LOW: return 2;
    case USB_HIGH: return 3;
    default: return 4;
    }
}

static int uspeed(int x)            /* xHCI speed id -> usbcore speed */
{
    switch (x) {
    case 1: return USB_FULL;
    case 2: return USB_LOW;
    case 3: return USB_HIGH;
    default: return USB_SUPER;
    }
}

/* Fill the slot context of the input context. */
static void fill_slot(struct usb_dev *ud, struct xdev *d, int maxdci)
{
    uint32_t *sl = ctx(d->in, 1);
    uint32_t s0 = (ud->route & 0xFFFFF) | ((uint32_t)xspeed(ud->speed) << 20) | ((uint32_t)maxdci << 27);
    uint32_t s1 = (uint32_t)ud->root_port << 16;
    uint32_t s2 = 0;

    if (ud->is_hub) {
        s0 |= 1u << 26;
        s1 |= (uint32_t)ud->nports << 24;
    }
    if (ud->speed < USB_HIGH) {                         /* find the transaction translator */
        for (struct usb_dev *p = ud; p->parent; p = p->parent)
            if (p->parent->speed == USB_HIGH) {
                struct xdev *h = p->parent->hcpriv;
                s2 = (uint32_t)h->slot | ((uint32_t)p->port << 8);
                break;
            }
    }
    sl[0] = s0;
    sl[1] = s1;
    sl[2] = s2;
}

static void ep_context(uint8_t *in, int dci, int type, int mps, int interval, paddr_t ring)
{
    uint32_t *e = ctx(in, dci + 1);

    e[0] = (uint32_t)interval << 16;
    e[1] = (3u << 1) | ((uint32_t)type << 3) | ((uint32_t)mps << 16);
    e[2] = ring | 1;
    e[3] = 0;
    e[4] = (uint32_t)mps | ((uint32_t)mps << 16);
}

static void free_dev(struct xdev *d)
{
    for (int e = 0; e < USB_MAX_EPS; e++)
        if (d->xe[e].ring.t)
            ring_free(&d->xe[e].ring);
    if (d->ep0.t)
        ring_free(&d->ep0);
    dma_free(d->out);
    dma_free(d->in);
    memset(d, 0, sizeof *d);
}

/* ------------------------------------------------------------------ */
/* usb_hc_ops                                                          */
/* ------------------------------------------------------------------ */

static int xhci_dev_alloc(struct usb_dev *ud)
{
    uint32_t slot = 0;
    int cc = command(0, CMD_ENABLE_SLOT << 10, &slot);

    if (cc != CC_SUCCESS || slot == 0 || slot > (uint32_t)X.maxslots) {
        log_warn("usb", "xHCI: no free device slot (code %d)", cc);
        return -1;
    }
    struct xdev *d = NULL;
    for (int i = 0; i < USB_MAX_DEVS; i++)
        if (!xdevs[i].used) {
            d = &xdevs[i];
            break;
        }
    if (!d) {
        command(0, (CMD_DISABLE_SLOT << 10) | (slot << 24), NULL);
        return -1;
    }
    memset(d, 0, sizeof *d);
    d->out = dma_page(&d->out_p);
    d->in = dma_page(&d->in_p);
    d->slot = (int)slot;
    d->ud = ud;
    d->used = true;
    ud->hcpriv = d;
    if (!d->out || !d->in || !ring_alloc(&d->ep0, RING_TRBS)) {
        command(0, (CMD_DISABLE_SLOT << 10) | (slot << 24), NULL);
        free_dev(d);
        ud->hcpriv = NULL;
        return -1;
    }
    ud->mps0 = ud->speed == USB_SUPER ? 512 : (ud->speed == USB_LOW ? 8 : 64);
    return 0;
}

static int xhci_dev_address(struct usb_dev *ud)
{
    struct xdev *d = ud->hcpriv;

    memset(d->in, 0, 4096);
    X.dcbaa[d->slot * 2] = d->out_p;
    X.dcbaa[d->slot * 2 + 1] = 0;
    uint32_t *ctl = ctx(d->in, 0);
    ctl[1] = 3;                                              /* A0 + A1 */
    fill_slot(ud, d, 1);
    uint32_t *e0 = ctx(d->in, 2);
    e0[1] = (3u << 1) | (4u << 3) | ((uint32_t)ud->mps0 << 16);
    e0[2] = d->ep0.phys | 1;
    e0[4] = 8;
    int cc = command(d->in_p, (CMD_ADDRESS_DEV << 10) | ((uint32_t)d->slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        log_warn("usb", "xHCI: address device failed (code %d)", cc);
        return -1;
    }
    return 0;
}

static int xhci_set_mps0(struct usb_dev *ud, int mps)
{
    struct xdev *d = ud->hcpriv;

    memset(d->in, 0, 4096);
    ctx(d->in, 0)[1] = 2;                                    /* A1: endpoint 0 only */
    uint32_t *e0 = ctx(d->in, 2);
    e0[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16);
    e0[2] = d->ep0.phys | 1;
    e0[4] = 8;
    return command(d->in_p, (CMD_EVAL_CTX << 10) | ((uint32_t)d->slot << 24), NULL) == CC_SUCCESS ? 0 : -1;
}

static void xhci_dev_free(struct usb_dev *ud)
{
    struct xdev *d = ud->hcpriv;

    if (!d)
        return;
    command(0, (CMD_DISABLE_SLOT << 10) | ((uint32_t)d->slot << 24), NULL);
    X.dcbaa[d->slot * 2] = 0;
    free_dev(d);
    ud->hcpriv = NULL;
}

/* Called once all endpoints of the device are known (and once for a hub). */
static int xhci_configure(struct usb_dev *ud)
{
    struct xdev *d = ud->hcpriv;
    int maxdci = 1;

    memset(d->in, 0, 4096);
    uint32_t *ctl = ctx(d->in, 0);
    ctl[1] = 1;                                              /* A0: slot context */
    for (int i = 0; i < USB_MAX_EPS; i++) {
        struct usb_ep *u = &ud->ep[i];
        struct xep *e = &d->xe[i];
        if (!u->used)
            continue;
        if (!ring_alloc(&e->ring, RING_TRBS))
            return -1;
        e->ue = u;
        u->hcpriv = e;
        e->dci = u->num * 2 + 1;
        e->mps = u->mps;
        int intv;
        if (ud->speed >= USB_HIGH) {
            intv = u->interval - 1;
            if (intv < 0) intv = 0;
            if (intv > 15) intv = 15;
        } else {
            intv = ilog2((uint32_t)(u->interval ? u->interval : 8)) + 3;
            if (intv < 3) intv = 3;
            if (intv > 10) intv = 10;
        }
        ep_context(d->in, e->dci, 7, e->mps, intv, e->ring.phys);
        ctl[1] |= 1u << e->dci;
        if (e->dci > maxdci)
            maxdci = e->dci;
    }
    fill_slot(ud, d, maxdci);
    int cc = command(d->in_p, (CMD_CONFIG_EP << 10) | ((uint32_t)d->slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        log_warn("usb", "xHCI: configure endpoint failed (code %d)", cc);
        return -1;
    }
    return 0;
}

static int xhci_intr_start(struct usb_dev *ud, struct usb_ep *u)
{
    requeue(ud->hcpriv, u->hcpriv);
    return 0;
}

/* ---- root hub ---- */

static volatile uint32_t *portsc(int port) { return (volatile uint32_t *)(X.op + 0x400 + 0x10 * (port - 1)); }

#define PORT_CCS  (1u << 0)
#define PORT_PED  (1u << 1)
#define PORT_PR   (1u << 4)
#define PORT_PP   (1u << 9)
#define PORT_PRC  (1u << 21)
#define PORT_RW1C 0x00FE0002u           /* change bits + PED: write 1 clears/disables */

static int xhci_rh_connected(struct usb_hc *hc, int port)
{
    (void)hc;
    uint32_t v = rd(portsc(port));

    if (v & 0x00FE0000u)                                     /* acknowledge change bits */
        wr(portsc(port), (v & ~PORT_RW1C) | (v & 0x00FE0000u & ~PORT_PRC));
    return (v & PORT_CCS) ? 1 : 0;
}

static int xhci_rh_reset(struct usb_hc *hc, int port)
{
    (void)hc;
    uint32_t v = rd(portsc(port));

    if (!((v & PORT_PED) && ((v >> 10) & 15) == 4)) {        /* super speed ports train themselves */
        wr(portsc(port), (v & ~PORT_RW1C) | PORT_PR);
        uint32_t start = timer_ticks();
        while (!(rd(portsc(port)) & PORT_PRC)) {
            if (timer_ticks() - start > 100)
                return -1;
            timer_sleep_ms(10);
        }
        v = rd(portsc(port));
        wr(portsc(port), (v & ~PORT_RW1C) | PORT_PRC);       /* acknowledge */
        timer_sleep_ms(20);                                  /* reset recovery time */
    }
    v = rd(portsc(port));
    if ((v & (PORT_PED | PORT_CCS)) != (PORT_PED | PORT_CCS))
        return -1;
    return uspeed((int)((v >> 10) & 15));
}

static const struct usb_hc_ops xhci_ops = {
    .name = "xHCI",
    .addr_first = true,
    .dev_alloc = xhci_dev_alloc,
    .dev_address = xhci_dev_address,
    .dev_free = xhci_dev_free,
    .set_mps0 = xhci_set_mps0,
    .ctrl = xhci_ctrl,
    .configure = xhci_configure,
    .intr_start = xhci_intr_start,
    .poll = xhci_poll,
    .rh_connected = xhci_rh_connected,
    .rh_reset = xhci_rh_reset,
};

/* ------------------------------------------------------------------ */
/* Controller start-up                                                 */
/* ------------------------------------------------------------------ */

static bool wait_reg(volatile uint8_t *reg, uint32_t mask, uint32_t want, int ms)
{
    uint32_t start = timer_ticks();

    while ((rd(reg) & mask) != want) {
        if ((timer_ticks() - start) * 10 > (uint32_t)ms)
            return false;
        timer_sleep_ms(1);
    }
    return true;
}

static void take_ownership(uint32_t hccparams1)
{
    uint32_t off = (hccparams1 >> 16) * 4;

    while (off) {
        volatile uint8_t *p = X.cap + off;
        uint32_t v = rd(p);
        if ((v & 0xFF) == 1) {                               /* USB Legacy Support */
            if (v & (1u << 16)) {                            /* BIOS still owns it */
                wr(p, v | (1u << 24));
                wait_reg(p, 1u << 16, 0, 1000);
            }
            volatile uint8_t *ctl = p + 4;                   /* switch off SMI generation */
            wr(ctl, (rd(ctl) & ~((1u << 0) | (1u << 4) | (0x7u << 13))) | 0xE0000000u);
            return;
        }
        uint32_t next = (v >> 8) & 0xFF;
        if (!next)
            return;
        off += next * 4;
    }
}

static bool controller_init(const struct pci_device *pd)
{
    uint8_t b = pd->bus, s = pd->slot, f = pd->func;
    uint32_t bar = pci_cfg_read(b, s, f, 0x10);

    if (bar & 1) {
        log_warn("usb", "xHCI BAR0 is an I/O range (unsupported)");
        return false;
    }
    if (((bar >> 1) & 3) == 2 && pci_cfg_read(b, s, f, 0x14) != 0) {
        log_warn("usb", "xHCI registers are above 4 GiB (unsupported)");
        return false;
    }
    uint32_t base = bar & ~0xFu;
    uint32_t cmd = pci_cfg_read(b, s, f, 0x04);
    pci_cfg_write(b, s, f, 0x04, (cmd & ~7u) );              /* decode off while probing the size */
    pci_cfg_write(b, s, f, 0x10, 0xFFFFFFFFu);
    uint32_t sz = pci_cfg_read(b, s, f, 0x10) & ~0xFu;
    pci_cfg_write(b, s, f, 0x10, bar);
    uint32_t size = sz ? (~sz + 1) : 0x10000;
    if (size < 0x4000 || size > 0x400000)
        size = 0x10000;
    pci_cfg_write(b, s, f, 0x04, (cmd & 0xFFFF) | 6u);       /* memory space + bus master */
    if (!base) {
        log_warn("usb", "xHCI has no memory address assigned");
        return false;
    }
    if (vmm_map_range(base, base, size, VMM_WRITE | VMM_NOCACHE) < 0) {
        log_warn("usb", "cannot map xHCI registers");
        return false;
    }
    X.cap = (volatile uint8_t *)base;
    uint32_t caplen = *(volatile uint8_t *)X.cap;
    uint32_t hcs1 = rd(X.cap + 4), hcs2 = rd(X.cap + 8), hcc1 = rd(X.cap + 0x10);
    X.op = X.cap + caplen;
    X.db = X.cap + (rd(X.cap + 0x14) & ~3u);
    X.rt = X.cap + (rd(X.cap + 0x18) & ~0x1Fu);
    X.maxslots = (int)(hcs1 & 0xFF);
    X.maxports = (int)(hcs1 >> 24);
    X.ctxsz = (hcc1 & 4) ? 64 : 32;
    if (X.maxslots > 32)
        X.maxslots = 32;
    take_ownership(hcc1);

    /* stop, then reset */
    wr(X.op, rd(X.op) & ~1u);
    if (!wait_reg(X.op + 4, 1, 1, 500))
        log_warn("usb", "controller did not halt");
    wr(X.op, rd(X.op) | 2u);
    if (!wait_reg(X.op, 2, 0, 1000) || !wait_reg(X.op + 4, 1u << 11, 0, 1000)) {
        log_warn("usb", "controller reset timed out");
        return false;
    }

    X.pagesize = (rd(X.op + 8) & 0xFFFF) << 12;
    if (X.pagesize != 4096) {
        log_warn("usb", "unsupported xHCI page size");
        return false;
    }
    paddr_t dc_p;
    X.dcbaa = dma_page(&dc_p);
    if (!X.dcbaa || !ring_alloc(&X.cmd, RING_TRBS))
        return false;

    uint32_t nscratch = ((hcs2 >> 27) & 31) | (((hcs2 >> 21) & 31) << 5);
    if (nscratch) {
        paddr_t sp;
        uint32_t *arr = dma_page(&sp);
        if (!arr || nscratch > 500)
            return false;
        for (uint32_t i = 0; i < nscratch; i++) {
            paddr_t pg;
            if (!dma_page(&pg))
                return false;
            arr[i * 2] = pg;
        }
        X.dcbaa[0] = sp;
    }

    paddr_t ev_p, erst_p;
    X.evt = dma_page(&ev_p);
    uint32_t *erst = dma_page(&erst_p);
    if (!X.evt || !erst)
        return false;
    X.evt_phys = ev_p;
    X.evt_idx = 0;
    X.evt_ccs = 1;
    erst[0] = ev_p;
    erst[1] = 0;
    erst[2] = 256;
    erst[3] = 0;

    wr(X.op + 0x38, (uint32_t)X.maxslots);                   /* CONFIG */
    wr64(X.op + 0x30, dc_p);                                 /* DCBAAP */
    wr64(X.op + 0x18, X.cmd.phys | 1);                       /* CRCR with cycle state 1 */
    volatile uint8_t *ir = X.rt + 0x20;
    wr(ir + 0x08, 1);                                        /* ERSTSZ */
    wr64(ir + 0x18, ev_p);                                   /* ERDP */
    wr64(ir + 0x10, erst_p);                                 /* ERSTBA (starts the ring) */
    wr(ir + 0x00, 2);                                        /* IMAN: interrupt enable (INTE stays off) */
    wr(X.op, rd(X.op) | 1u);                                 /* run */
    if (!wait_reg(X.op + 4, 1, 0, 500)) {
        log_warn("usb", "controller did not start");
        return false;
    }
    for (int p = 1; p <= X.maxports; p++) {                  /* power the ports */
        uint32_t v = rd(portsc(p));
        if (!(v & PORT_PP))
            wr(portsc(p), (v & ~PORT_RW1C) | PORT_PP);
    }
    timer_sleep_ms(100);
    X.ok = true;
    return true;
}

bool xhci_probe(const struct pci_device *pd)
{
    if (X.ok)                                                /* only the first controller is used */
        return false;
    if (!controller_init(pd))
        return false;
    log_info("usb", "xHCI %04x:%04x ready: %d ports, %d slots, %d-byte contexts",
             pd->vendor, pd->device, X.maxports, X.maxslots, X.ctxsz);
    return usb_hc_add(&xhci_ops, "xHCI", X.maxports, &X) >= 0;
}
