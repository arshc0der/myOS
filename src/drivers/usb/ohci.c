/*
 * ohci.c - USB 1.1 "OHCI" host controller driver (memory-mapped; AMD, NEC,
 * Apple and most non-Intel boards, and the companion controllers of EHCI).
 *
 * How it works, in short:
 *   - Software and controller share a "HCCA" page. Its 32 interrupt-table
 *     entries all point at the same chain of "endpoint descriptors" (EDs),
 *     one per interrupt endpoint (keyboard, mouse); the chain is walked
 *     every millisecond.
 *   - An ED points at a queue of "transfer descriptors" (TDs): HeadP is the
 *     next TD to run, TailP is a dummy TD that marks the end of the queue.
 *   - Control transfers use one ED on the control list; its TD queue is
 *     rebuilt for every transfer (SETUP, DATA..., STATUS).
 *   - No interrupts are used; usbcore.c polls and looks at the TD
 *     condition codes (0xF = the controller has not touched it yet).
 */
#include "usbcore.h"
#include "sched.h"
#include "timer.h"
#include "kstring.h"
#include "log.h"

#define HcControl        0x04
#define HcCommandStatus  0x08
#define HcIntStatus      0x0C
#define HcIntDisable     0x14
#define HcHCCA           0x18
#define HcControlHeadED  0x20
#define HcBulkHeadED     0x28
#define HcFmInterval     0x34
#define HcPeriodicStart  0x40
#define HcLSThreshold    0x44
#define HcRhDescriptorA  0x48
#define HcRhStatus       0x50
#define HcRhPortStatus(n) (0x54 + 4 * ((n) - 1))

#define CTRL_PLE  (1u << 2)
#define CTRL_CLE  (1u << 4)
#define CTRL_IR   (1u << 8)
#define CTRL_OPER (2u << 6)
#define CMD_HCR   (1u << 0)
#define CMD_CLF   (1u << 1)
#define CMD_OCR   (1u << 3)

#define ED_SKIP   (1u << 14)
#define ED_LOW    (1u << 13)
#define ED_DIR_IN (2u << 11)

#define TD_R      (1u << 18)
#define TD_DP_SETUP (0u << 19)
#define TD_DP_OUT   (1u << 19)
#define TD_DP_IN    (2u << 19)
#define TD_NOINT    (7u << 21)
#define TD_DATA0    (2u << 24)
#define TD_DATA1    (3u << 24)
#define TD_CC(x)    ((uint32_t)(x) << 28)

#define PS_CCS  (1u << 0)
#define PS_PES  (1u << 1)
#define PS_PRS  (1u << 4)
#define PS_PPS  (1u << 8)
#define PS_LSDA (1u << 9)
#define PS_PRSC (1u << 20)

#define MAX_INTR 16
#define MAX_TDS  192

struct ed { volatile uint32_t flags, tailp, headp, next; };
struct td { volatile uint32_t flags, cbp, next, be; };

struct ohci_ep {
    bool used;
    int idx;                    /* ED index */
    int cur;                    /* 0/1: which of the two TDs is the active one */
    int toggle, errors;
    struct td *td[2];
    struct usb_ep *ue;
    struct usb_dev *ud;
};

struct ohci {
    volatile uint8_t *regs;
    uint8_t *hcca;
    struct ed *ed;              /* [0] = control, [1..MAX_INTR] = interrupt EDs */
    struct td *tds;             /* control TDs, then 2 per interrupt endpoint */
    struct ohci_ep ep[MAX_INTR];
    uint8_t addr_used[16];
    int nports;
};

static struct ohci O[6];
static int nohci;

static inline uint32_t rd(struct ohci *o, int off) { return *(volatile uint32_t *)(o->regs + off); }
static inline void wr(struct ohci *o, int off, uint32_t v) { *(volatile uint32_t *)(o->regs + off) = v; }
static inline void barrier(void) { __asm__ volatile("" ::: "memory"); }
static inline uint32_t phys(const void *p) { return (uint32_t)(paddr_t)p; }

static void ack_done(struct ohci *o)
{
    uint32_t s = rd(o, HcIntStatus);

    if (s)
        wr(o, HcIntStatus, s);                              /* we look at the TDs, not at the done queue */
}

static int ohci_ctrl(struct usb_dev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len)
{
    struct ohci *o = d->hc->priv;
    struct ed *ed = &o->ed[0];
    bool in = (rt & 0x80) != 0;
    int mps = d->mps0 ? d->mps0 : 8;
    uint8_t *setup = d->buf + 2048;
    int n = 0, ndata = 0;

    if (len > 1024)
        return -1;
    setup[0] = rt;
    setup[1] = req;
    setup[2] = (uint8_t)val;
    setup[3] = (uint8_t)(val >> 8);
    setup[4] = (uint8_t)idx;
    setup[5] = (uint8_t)(idx >> 8);
    setup[6] = (uint8_t)len;
    setup[7] = (uint8_t)(len >> 8);

    struct td *t = &o->tds[n++];
    t->flags = TD_DP_SETUP | TD_NOINT | TD_DATA0 | TD_CC(0xF);
    t->cbp = phys(setup);
    t->be = phys(setup) + 7;
    int toggle = 1;
    for (int off = 0; off < len; off += mps) {
        int chunk = len - off < mps ? len - off : mps;
        if (n >= MAX_TDS - 2)
            return -1;
        t = &o->tds[n++];
        t->flags = (in ? TD_DP_IN | TD_R : TD_DP_OUT) | TD_NOINT | (toggle ? TD_DATA1 : TD_DATA0) | TD_CC(0xF);
        t->cbp = phys(d->buf + off);
        t->be = phys(d->buf + off) + (uint32_t)chunk - 1;
        toggle ^= 1;
        ndata++;
    }
    t = &o->tds[n++];                                       /* status stage */
    t->flags = ((len && in) ? TD_DP_OUT : TD_DP_IN) | TD_NOINT | TD_DATA1 | TD_CC(0xF);
    t->cbp = 0;
    t->be = 0;
    struct td *tail = &o->tds[n];                           /* dummy end-of-queue TD */
    memset(tail, 0, sizeof *tail);
    for (int i = 0; i < n; i++)
        o->tds[i].next = phys(&o->tds[i + 1]);

    ed->flags = (uint32_t)d->addr | ((uint32_t)mps << 16) | ED_SKIP | (d->speed == USB_LOW ? ED_LOW : 0);
    ed->tailp = phys(tail);
    ed->headp = phys(&o->tds[0]);
    barrier();
    ed->flags &= ~ED_SKIP;
    barrier();
    wr(o, HcCommandStatus, CMD_CLF);

    uint32_t start = timer_ticks();
    bool skipped = false;
    int rc = -1;
    for (int spin = 0;; spin++) {
        bool failed = false, done = true;
        ack_done(o);
        for (int i = 0; i < n; i++) {
            uint32_t cc = o->tds[i].flags >> 28;
            if (cc == 0xF) {
                done = false;
                break;
            }
            if (cc != 0) {
                failed = true;
                break;
            }
            if (in && i >= 1 && i <= ndata && i < n - 2 && !skipped && o->tds[i].cbp != 0) {
                skipped = true;                             /* short packet: go straight to the status stage */
                ed->flags |= ED_SKIP;
                barrier();
                ed->headp = phys(&o->tds[n - 1]) | (ed->headp & 2u);
                barrier();
                ed->flags &= ~ED_SKIP;
                for (int k = i + 1; k < n - 1; k++)
                    o->tds[k].flags = (o->tds[k].flags & 0x0FFFFFFFu) | TD_CC(0);
            }
        }
        if (failed)
            break;
        if (done) {
            rc = 0;
            break;
        }
        if ((uint32_t)(timer_ticks() - start) > 50)         /* 500 ms */
            break;
        if (spin > 200)
            task_yield();
    }
    ed->flags |= ED_SKIP;
    if (rc < 0)
        timer_sleep_ms(10);
    ed->headp = phys(tail);
    ed->tailp = phys(tail);
    return rc;
}

static int ohci_dev_alloc(struct usb_dev *d)
{
    d->addr = 0;
    d->mps0 = 8;
    return 0;
}

static int ohci_dev_address(struct usb_dev *d)
{
    struct ohci *o = d->hc->priv;
    int a;

    for (a = 1; a < 128; a++)
        if (!(o->addr_used[a >> 3] & (1u << (a & 7))))
            break;
    if (a >= 128)
        return -1;
    if (ohci_ctrl(d, 0x00, 5, (uint16_t)a, 0, 0) < 0)
        return -1;
    timer_sleep_ms(10);
    d->addr = a;
    o->addr_used[a >> 3] |= (uint8_t)(1u << (a & 7));
    return 0;
}

static void ohci_dev_free(struct usb_dev *d)
{
    struct ohci *o = d->hc->priv;

    if (d->addr)
        o->addr_used[d->addr >> 3] &= (uint8_t)~(1u << (d->addr & 7));
}

/* Queue one IN TD on the endpoint: the idle TD becomes the active one. */
static void arm(struct ohci_ep *e)
{
    struct usb_dev *d = e->ud;
    struct td *act = e->td[e->cur], *dummy = e->td[e->cur ^ 1];
    struct ohci *o = d->hc->priv;
    struct ed *ed = &o->ed[e->idx];

    memset(dummy, 0, sizeof *dummy);
    act->flags = TD_DP_IN | TD_R | TD_NOINT | (e->toggle ? TD_DATA1 : TD_DATA0) | TD_CC(0xF);
    act->cbp = phys(e->ue->buf);
    act->be = phys(e->ue->buf) + (uint32_t)e->ue->mps - 1;
    act->next = phys(dummy);
    barrier();
    ed->tailp = phys(dummy);
    if (ed->headp & 1u)                                     /* halted by an error: restart at this TD */
        ed->headp = phys(act);
    barrier();
}

static int ohci_intr_start(struct usb_dev *d, struct usb_ep *ue)
{
    struct ohci *o = d->hc->priv;

    for (int i = 0; i < MAX_INTR; i++) {
        struct ohci_ep *e = &o->ep[i];
        if (e->used)
            continue;
        memset(e, 0, sizeof *e);
        e->used = true;
        e->idx = i + 1;
        e->td[0] = &o->tds[MAX_TDS + 2 * i];
        e->td[1] = &o->tds[MAX_TDS + 2 * i + 1];
        e->ue = ue;
        e->ud = d;
        ue->hcpriv = e;
        struct ed *ed = &o->ed[e->idx];
        ed->flags = (uint32_t)d->addr | ((uint32_t)ue->num << 7) | ED_DIR_IN | ((uint32_t)ue->mps << 16) |
                    (d->speed == USB_LOW ? ED_LOW : 0) | ED_SKIP;
        ed->headp = phys(e->td[1]);                         /* empty queue: head == tail */
        ed->tailp = phys(e->td[1]);
        e->cur = 1;
        arm(e);                                             /* active TD = td[1], dummy = td[0] */
        ed->headp = phys(e->td[1]);
        barrier();
        ed->flags &= ~ED_SKIP;
        return 0;
    }
    return -1;
}

static void ohci_intr_stop(struct usb_dev *d, struct usb_ep *ue)
{
    struct ohci *o = d->hc->priv;
    struct ohci_ep *e = ue->hcpriv;

    if (!e)
        return;
    o->ed[e->idx].flags |= ED_SKIP;
    timer_sleep_ms(10);
    e->used = false;
    ue->hcpriv = NULL;
}

static void ohci_poll(struct usb_hc *hc)
{
    struct ohci *o = hc->priv;

    ack_done(o);
    for (int i = 0; i < MAX_INTR; i++) {
        struct ohci_ep *e = &o->ep[i];
        if (!e->used)
            continue;
        struct td *act = e->td[e->cur];
        uint32_t cc = act->flags >> 28;
        if (cc == 0xF)
            continue;                                       /* nothing yet (NAKs are retried by the controller) */
        if (cc == 0) {
            int got = act->cbp ? (int)(act->cbp - phys(e->ue->buf)) : e->ue->mps;
            e->errors = 0;
            e->toggle ^= 1;
            if (got > 0)
                usb_report(e->ud, e->ue, e->ue->buf, got);
        } else if (++e->errors >= 8) {
            log_warn("usb", "OHCI: endpoint %d keeps failing (code %u), stopped", e->ue->num, cc);
            o->ed[e->idx].flags |= ED_SKIP;
            e->used = false;
            continue;
        }
        e->cur ^= 1;
        arm(e);
    }
}

static int ohci_rh_connected(struct usb_hc *hc, int port)
{
    struct ohci *o = hc->priv;
    uint32_t v = rd(o, HcRhPortStatus(port));

    if (v & 0x1F0000u)
        wr(o, HcRhPortStatus(port), v & 0x1F0000u);         /* write 1 to clear the change bits */
    return (v & PS_CCS) ? 1 : 0;
}

static int ohci_rh_reset(struct usb_hc *hc, int port)
{
    struct ohci *o = hc->priv;

    wr(o, HcRhPortStatus(port), PS_PRS);
    uint32_t start = timer_ticks();
    while (!(rd(o, HcRhPortStatus(port)) & PS_PRSC)) {
        if (timer_ticks() - start > 100)
            return -1;
        timer_sleep_ms(10);
    }
    wr(o, HcRhPortStatus(port), PS_PRSC);
    timer_sleep_ms(20);
    uint32_t v = rd(o, HcRhPortStatus(port));
    if (!(v & PS_PES)) {
        wr(o, HcRhPortStatus(port), PS_PES);
        timer_sleep_ms(10);
        v = rd(o, HcRhPortStatus(port));
    }
    if (!(v & PS_CCS) || !(v & PS_PES))
        return -1;
    return (v & PS_LSDA) ? USB_LOW : USB_FULL;
}

static const struct usb_hc_ops ohci_ops = {
    .name = "OHCI",
    .addr_first = false,
    .dev_alloc = ohci_dev_alloc,
    .dev_address = ohci_dev_address,
    .dev_free = ohci_dev_free,
    .ctrl = ohci_ctrl,
    .intr_start = ohci_intr_start,
    .intr_stop = ohci_intr_stop,
    .poll = ohci_poll,
    .rh_connected = ohci_rh_connected,
    .rh_reset = ohci_rh_reset,
};

bool ohci_probe(const struct pci_device *pd)
{
    if (nohci >= 6)
        return false;
    struct ohci *o = &O[nohci];
    memset(o, 0, sizeof *o);
    o->regs = usb_map_bar0(pd);
    if (!o->regs)
        return false;

    /* take the controller over from the BIOS if its system-management code still runs it */
    if (rd(o, HcControl) & CTRL_IR) {
        wr(o, HcCommandStatus, CMD_OCR);
        for (int i = 0; i < 100 && (rd(o, HcControl) & CTRL_IR); i++)
            timer_sleep_ms(10);
    }
    uint32_t fm = rd(o, HcFmInterval);
    wr(o, HcIntDisable, 0xC000007Fu);
    wr(o, HcCommandStatus, CMD_HCR);
    for (int i = 0; i < 20 && (rd(o, HcCommandStatus) & CMD_HCR); i++)
        timer_sleep_ms(1);
    if (rd(o, HcCommandStatus) & CMD_HCR) {
        log_warn("usb", "OHCI: reset did not finish");
        return false;
    }

    paddr_t hp, ep_, tp;
    o->hcca = usb_dma_pages(1, &hp);
    o->ed = usb_dma_pages(1, &ep_);
    o->tds = usb_dma_pages(2, &tp);
    if (!o->hcca || !o->ed || !o->tds)
        return false;
    /* EDs: [0] control, [1..] interrupt chain */
    o->ed[0].flags = ED_SKIP;
    for (int i = 1; i <= MAX_INTR; i++) {
        o->ed[i].flags = ED_SKIP;
        o->ed[i].next = i < MAX_INTR ? (uint32_t)(ep_ + (paddr_t)(i + 1) * sizeof(struct ed)) : 0;
    }
    for (int i = 0; i < 32; i++)
        ((uint32_t *)o->hcca)[i] = (uint32_t)(ep_ + sizeof(struct ed));

    uint32_t fi = fm & 0x3FFF;
    if (fi < 0x400)
        fi = 0x2EDF;
    wr(o, HcHCCA, (uint32_t)hp);
    wr(o, HcControlHeadED, (uint32_t)ep_);
    wr(o, HcBulkHeadED, 0);
    wr(o, HcFmInterval, fi | ((((fi - 210) * 6) / 7) << 16) | ((fm & 0x80000000u) ^ 0x80000000u));
    wr(o, HcPeriodicStart, (fi * 9) / 10);
    wr(o, HcLSThreshold, 0x628);
    wr(o, HcIntStatus, 0x7F);
    wr(o, HcControl, CTRL_OPER | CTRL_PLE | CTRL_CLE | 3u);
    timer_sleep_ms(10);

    uint32_t da = rd(o, HcRhDescriptorA);
    int np = (int)(da & 0xFF);
    if (np > 15)
        np = 15;
    wr(o, HcRhStatus, 1u << 16);                            /* power all ports */
    for (int p = 1; p <= np; p++)
        wr(o, HcRhPortStatus(p), PS_PPS);
    uint32_t pw = ((da >> 24) & 0xFF) * 2;
    timer_sleep_ms((pw < 20 ? 20 : pw) + 20);
    o->nports = np;
    log_info("usb", "OHCI %04x:%04x ready: %d ports", pd->vendor, pd->device, np);
    if (usb_hc_add(&ohci_ops, "OHCI", np, o) < 0)
        return false;
    nohci++;
    return true;
}
