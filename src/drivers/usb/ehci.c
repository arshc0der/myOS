/*
 * ehci.c - USB 2.0 "EHCI" host controller driver (high speed, memory-mapped).
 *
 * How it works, in short:
 *   - EHCI only speaks high speed (480 Mbit/s). Low and full speed devices
 *     (most keyboards and mice!) are handed over to the "companion"
 *     controller (UHCI or OHCI) by setting the port's "owner" bit; those
 *     controllers are driven by uhci.c / ohci.c.
 *   - Control transfers use one queue head on the "async" list with three
 *     queue transfer descriptors (qTDs): SETUP, DATA, STATUS.
 *   - Interrupt endpoints (high speed keyboards/mice/touchpads) own one
 *     queue head each on the "periodic" list, which is walked every frame.
 *   - Not supported: low/full speed devices behind a high speed hub (they
 *     need "split transactions"); usbcore.c prints a message for those.
 *   - No interrupts are used; usbcore.c polls.
 */
#include "usbcore.h"
#include "sched.h"
#include "timer.h"
#include "kstring.h"
#include "log.h"

/* operational registers (offsets from the operational base) */
#define USBCMD     0x00
#define USBSTS     0x04
#define USBINTR    0x08
#define FRINDEX    0x0C
#define CTRLDSSEG  0x10
#define PERIODICBASE 0x14
#define ASYNCBASE  0x18
#define CONFIGFLAG 0x40
#define PORTSC(n)  (0x44 + 4 * ((n) - 1))

#define CMD_RUN    (1u << 0)
#define CMD_RESET  (1u << 1)
#define CMD_PSE    (1u << 4)
#define CMD_ASE    (1u << 5)
#define STS_HALTED (1u << 12)

#define PS_CCS   (1u << 0)
#define PS_CSC   (1u << 1)
#define PS_PED   (1u << 2)
#define PS_PEDC  (1u << 3)
#define PS_OCC   (1u << 5)
#define PS_RESET (1u << 8)
#define PS_POWER (1u << 12)
#define PS_OWNER (1u << 13)
#define PS_CHG   (PS_CSC | PS_PEDC | PS_OCC)

#define LP_T     1u
#define LP_QH    2u

#define QT_ACTIVE 0x80u
#define QT_HALT   0x40u
#define QT_ERR    0x78u         /* data buffer, babble, transaction error, missed micro-frame */
#define QT_PID_OUT   (0u << 8)
#define QT_PID_IN    (1u << 8)
#define QT_PID_SETUP (2u << 8)
#define QT_CERR3  (3u << 10)
#define QT_IOC    (1u << 15)
#define QT_TOGGLE (1u << 31)

#define MAX_INTR 16

struct qtd {
    volatile uint32_t next, alt, token, buf[5];
};

struct qh {
    volatile uint32_t hlink, chars, caps, cur;
    volatile uint32_t nxt, alt, token, buf[5];
    uint32_t pad[4];
};

struct ehci_ep {
    bool used;
    int idx;
    int toggle, errors;
    struct qtd *qt, *qt2;       /* two qTDs, used alternately */
    struct usb_ep *ue;
    struct usb_dev *ud;
};

struct ehci {
    volatile uint8_t *cap, *op;
    uint32_t *frames;
    struct qh *qh;              /* [0] control, [1..MAX_INTR] interrupt */
    paddr_t qh_p;
    struct qtd *qt;             /* [0..2] control, [3..] one per interrupt endpoint */
    struct ehci_ep ep[MAX_INTR];
    uint8_t addr_used[16];
    int nports;
};

static struct ehci E;
static bool ehci_ok;

static inline uint32_t rd(volatile uint8_t *p) { return *(volatile uint32_t *)p; }
static inline void wr(volatile uint8_t *p, uint32_t v) { *(volatile uint32_t *)p = v; }
static inline void barrier(void) { __asm__ volatile("" ::: "memory"); }
static inline uint32_t phys(const void *p) { return (uint32_t)(paddr_t)p; }

static void fill_qtd(struct qtd *q, uint32_t next, uint32_t alt, uint32_t token, const void *buf)
{
    q->next = next;
    q->alt = alt;
    for (int i = 0; i < 5; i++)
        q->buf[i] = 0;
    if (buf) {
        uint32_t a = phys(buf);
        q->buf[0] = a;
        q->buf[1] = (a & ~0xFFFu) + 0x1000;
    }
    barrier();
    q->token = token;
}

static int ehci_ctrl(struct usb_dev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len)
{
    struct ehci *e = d->hc->priv;
    struct qh *qh = &e->qh[0];
    bool in = (rt & 0x80) != 0;
    uint8_t *setup = d->buf + 2048;

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

    struct qtd *s = &e->qt[0], *dt = &e->qt[1], *st = &e->qt[2];
    /* build back to front so the links are valid */
    fill_qtd(st, LP_T, LP_T, QT_ACTIVE | QT_CERR3 | QT_IOC | QT_TOGGLE | ((len && in) ? QT_PID_OUT : QT_PID_IN), NULL);
    if (len)
        fill_qtd(dt, phys(st), phys(st), QT_ACTIVE | QT_CERR3 | QT_TOGGLE | ((uint32_t)len << 16) |
                 (in ? QT_PID_IN : QT_PID_OUT), d->buf);
    fill_qtd(s, len ? phys(dt) : phys(st), LP_T, QT_ACTIVE | QT_CERR3 | (8u << 16) | QT_PID_SETUP, setup);

    qh->chars = (uint32_t)d->addr | (2u << 12) | (1u << 14) | (1u << 15) | ((uint32_t)(d->mps0 ? d->mps0 : 64) << 16) | (4u << 28);
    qh->caps = 1u << 30;
    qh->cur = 0;
    qh->alt = LP_T;
    qh->token = 0;
    barrier();
    qh->nxt = phys(s);                                      /* the controller picks the transfer up from here */

    uint32_t start = timer_ticks();
    int rc = -1;
    for (int spin = 0;; spin++) {
        bool failed = false;
        struct qtd *list[3] = { s, len ? dt : NULL, st };
        for (int i = 0; i < 3; i++)
            if (list[i] && (list[i]->token & QT_HALT))
                failed = true;
        if (failed)
            break;
        if (!(st->token & QT_ACTIVE)) {
            rc = 0;
            break;
        }
        if ((uint32_t)(timer_ticks() - start) > 50)
            break;
        if (spin > 200)
            task_yield();
    }
    if (rc < 0) {
        qh->nxt = LP_T;
        qh->token = 0;
        timer_sleep_ms(10);
    }
    return rc;
}

static int ehci_dev_alloc(struct usb_dev *d)
{
    d->addr = 0;
    return 0;
}

static int ehci_dev_address(struct usb_dev *d)
{
    struct ehci *e = d->hc->priv;
    int a;

    for (a = 1; a < 128; a++)
        if (!(e->addr_used[a >> 3] & (1u << (a & 7))))
            break;
    if (a >= 128)
        return -1;
    if (ehci_ctrl(d, 0x00, 5, (uint16_t)a, 0, 0) < 0)
        return -1;
    timer_sleep_ms(10);
    d->addr = a;
    e->addr_used[a >> 3] |= (uint8_t)(1u << (a & 7));
    return 0;
}

static void ehci_dev_free(struct usb_dev *d)
{
    struct ehci *e = d->hc->priv;

    if (d->addr)
        e->addr_used[d->addr >> 3] &= (uint8_t)~(1u << (d->addr & 7));
}

static void arm(struct ehci_ep *p)
{
    struct qtd *tmp = p->qt;

    p->qt = p->qt2;                                         /* a new qTD address every time */
    p->qt2 = tmp;
    struct ehci *e = p->ud->hc->priv;
    struct qh *qh = &e->qh[p->idx];

    fill_qtd(p->qt, LP_T, LP_T, QT_ACTIVE | QT_CERR3 | QT_IOC | QT_PID_IN | ((uint32_t)p->ue->mps << 16) |
             (p->toggle ? QT_TOGGLE : 0), p->ue->buf);
    qh->token = 0;
    barrier();
    qh->nxt = phys(p->qt);
}

static int ehci_intr_start(struct usb_dev *d, struct usb_ep *ue)
{
    struct ehci *e = d->hc->priv;

    for (int i = 0; i < MAX_INTR; i++) {
        struct ehci_ep *p = &e->ep[i];
        if (p->used)
            continue;
        memset(p, 0, sizeof *p);
        p->used = true;
        p->idx = i + 1;
        p->qt = &e->qt[3 + 2 * i];
        p->qt2 = &e->qt[4 + 2 * i];
        p->ue = ue;
        p->ud = d;
        ue->hcpriv = p;
        struct qh *qh = &e->qh[p->idx];
        qh->chars = (uint32_t)d->addr | ((uint32_t)ue->num << 8) | (2u << 12) | (1u << 14) | ((uint32_t)ue->mps << 16);
        qh->caps = 0x01u | (1u << 30);                      /* poll in micro-frame 0 of every frame */
        arm(p);
        return 0;
    }
    return -1;
}

static void ehci_intr_stop(struct usb_dev *d, struct usb_ep *ue)
{
    struct ehci *e = d->hc->priv;
    struct ehci_ep *p = ue->hcpriv;

    if (!p)
        return;
    struct qh *qh = &e->qh[p->idx];
    qh->nxt = LP_T;
    qh->token = 0;
    qh->chars = 0;                                          /* address 0 / endpoint 0: never matches a transfer */
    timer_sleep_ms(10);
    p->used = false;
    ue->hcpriv = NULL;
}

static void ehci_poll(struct usb_hc *hc)
{
    struct ehci *e = hc->priv;

    for (int i = 0; i < MAX_INTR; i++) {
        struct ehci_ep *p = &e->ep[i];
        if (!p->used)
            continue;
        uint32_t t = p->qt->token;
        if (t & QT_ACTIVE)
            continue;
        if (t & (QT_HALT | QT_ERR)) {
            if (++p->errors >= 8) {
                log_warn("usb", "EHCI: endpoint %d keeps failing (token %08x), stopped", p->ue->num, t);
                e->qh[p->idx].nxt = LP_T;
                p->used = false;
                continue;
            }
        } else {
            int got = p->ue->mps - (int)((t >> 16) & 0x7FFF);
            p->errors = 0;
            p->toggle ^= 1;
            if (got > 0)
                usb_report(p->ud, p->ue, p->ue->buf, got);
        }
        arm(p);
    }
}

static int ehci_rh_connected(struct usb_hc *hc, int port)
{
    struct ehci *e = hc->priv;
    uint32_t v = rd(e->op + PORTSC(port));

    if (v & PS_CHG)
        wr(e->op + PORTSC(port), (v & ~PS_CHG & ~PS_PED) | (v & PS_PED) | (v & PS_CHG));
    return ((v & PS_CCS) && !(v & PS_OWNER)) ? 1 : 0;
}

static int ehci_rh_reset(struct usb_hc *hc, int port)
{
    struct ehci *e = hc->priv;
    volatile uint8_t *ps = e->op + PORTSC(port);
    uint32_t v = rd(ps);

    if (((v >> 10) & 3) == 1) {                             /* K state: a low speed device */
        wr(ps, (v & ~PS_CHG) | PS_OWNER);
        return -2;
    }
    wr(ps, (v & ~(PS_CHG | PS_PED)) | PS_RESET);
    timer_sleep_ms(50);
    v = rd(ps);
    wr(ps, v & ~(PS_CHG | PS_PED | PS_RESET));
    for (int i = 0; i < 10 && (rd(ps) & PS_RESET); i++)
        timer_sleep_ms(10);
    timer_sleep_ms(10);
    v = rd(ps);
    if (!(v & PS_PED)) {                                    /* full speed: give it to the companion controller */
        wr(ps, (v & ~PS_CHG) | PS_OWNER);
        return -2;
    }
    return USB_HIGH;
}

static const struct usb_hc_ops ehci_ops = {
    .name = "EHCI",
    .addr_first = false,
    .no_split = true,
    .dev_alloc = ehci_dev_alloc,
    .dev_address = ehci_dev_address,
    .dev_free = ehci_dev_free,
    .ctrl = ehci_ctrl,
    .intr_start = ehci_intr_start,
    .intr_stop = ehci_intr_stop,
    .poll = ehci_poll,
    .rh_connected = ehci_rh_connected,
    .rh_reset = ehci_rh_reset,
};

static bool wait_reg(volatile uint8_t *reg, uint32_t mask, uint32_t want, int ms)
{
    for (int i = 0; i < ms; i++) {
        if ((rd(reg) & mask) == want)
            return true;
        timer_sleep_ms(1);
    }
    return (rd(reg) & mask) == want;
}

bool ehci_probe(const struct pci_device *pd)
{
    if (ehci_ok)
        return false;
    memset(&E, 0, sizeof E);
    E.cap = usb_map_bar0(pd);
    if (!E.cap)
        return false;
    uint32_t hcs = rd(E.cap + 4), hcc = rd(E.cap + 8);
    E.op = E.cap + (*(volatile uint8_t *)E.cap);
    E.nports = (int)(hcs & 15);

    /* take the controller over from the BIOS */
    uint32_t eecp = (hcc >> 8) & 0xFF;
    while (eecp >= 0x40) {
        uint32_t v = pci_cfg_read(pd->bus, pd->slot, pd->func, (uint8_t)eecp);
        if ((v & 0xFF) == 1) {
            if (v & (1u << 16)) {
                pci_cfg_write(pd->bus, pd->slot, pd->func, (uint8_t)eecp, v | (1u << 24));
                for (int i = 0; i < 100; i++) {
                    if (!(pci_cfg_read(pd->bus, pd->slot, pd->func, (uint8_t)eecp) & (1u << 16)))
                        break;
                    timer_sleep_ms(10);
                }
            }
            pci_cfg_write(pd->bus, pd->slot, pd->func, (uint8_t)(eecp + 4), 0);   /* no SMIs */
            break;
        }
        eecp = (v >> 8) & 0xFF;
    }
    wr(E.op + USBCMD, rd(E.op + USBCMD) & ~CMD_RUN);
    if (!wait_reg(E.op + USBSTS, STS_HALTED, STS_HALTED, 200))
        log_warn("usb", "EHCI did not halt");
    wr(E.op + USBCMD, rd(E.op + USBCMD) | CMD_RESET);
    if (!wait_reg(E.op + USBCMD, CMD_RESET, 0, 500)) {
        log_warn("usb", "EHCI reset timed out");
        return false;
    }
    wr(E.op + USBINTR, 0);
    wr(E.op + CTRLDSSEG, 0);

    paddr_t fp, qp, tp;
    E.frames = usb_dma_pages(1, &fp);
    E.qh = usb_dma_pages(1, &qp);
    E.qt = usb_dma_pages(1, &tp);
    if (!E.frames || !E.qh || !E.qt)
        return false;
    E.qh_p = qp;
    /* control QH: head of the async list, points to itself */
    E.qh[0].hlink = (uint32_t)qp | LP_QH;
    E.qh[0].chars = (1u << 15);
    E.qh[0].nxt = LP_T;
    E.qh[0].alt = LP_T;
    /* interrupt QHs, chained, all idle */
    for (int i = 1; i <= MAX_INTR; i++) {
        E.qh[i].hlink = i < MAX_INTR ? ((uint32_t)(qp + (paddr_t)(i + 1) * sizeof(struct qh)) | LP_QH) : LP_T;
        E.qh[i].nxt = LP_T;
        E.qh[i].alt = LP_T;
    }
    for (int i = 0; i < 1024; i++)
        E.frames[i] = (uint32_t)(qp + sizeof(struct qh)) | LP_QH;
    wr(E.op + PERIODICBASE, (uint32_t)fp);
    wr(E.op + ASYNCBASE, (uint32_t)qp);
    wr(E.op + USBSTS, 0x3F);
    wr(E.op + USBCMD, CMD_RUN | CMD_ASE | CMD_PSE | (8u << 16));
    if (!wait_reg(E.op + USBSTS, STS_HALTED, 0, 200)) {
        log_warn("usb", "EHCI did not start");
        return false;
    }
    wr(E.op + CONFIGFLAG, 1);                               /* route every port to EHCI */
    timer_sleep_ms(10);
    if (hcs & (1u << 4))                                    /* port power control */
        for (int p = 1; p <= E.nports; p++)
            wr(E.op + PORTSC(p), (rd(E.op + PORTSC(p)) & ~PS_CHG) | PS_POWER);
    timer_sleep_ms(100);
    log_info("usb", "EHCI %04x:%04x ready: %d ports", pd->vendor, pd->device, E.nports);
    if (usb_hc_add(&ehci_ops, "EHCI", E.nports, &E) < 0)
        return false;
    ehci_ok = true;
    return true;
}
