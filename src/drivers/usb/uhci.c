/*
 * uhci.c - USB 1.1 "UHCI" host controller driver (Intel / VIA style, I/O ports).
 *
 * How it works, in short:
 *   - The controller walks a "frame list" (1024 pointers) once per
 *     millisecond. Every entry points at the same chain of "queue heads":
 *         interrupt QH 0 -> interrupt QH 1 -> ... -> control QH
 *   - A queue head points at a list of "transfer descriptors" (TDs). Each
 *     TD describes one packet. The controller clears the Active bit in a TD
 *     when it has finished it.
 *   - Control transfers are one TD chain (SETUP, DATA..., STATUS) hung on
 *     the control QH. Every interrupt endpoint (keyboard, mouse) owns one
 *     interrupt QH with a single IN TD that the usb task re-arms.
 *   - No interrupts are used; usbcore.c polls.
 */
#include "usbcore.h"
#include "io.h"
#include "sched.h"
#include "timer.h"
#include "kstring.h"
#include "log.h"

#define USBCMD    0x00
#define USBSTS    0x02
#define USBINTR   0x04
#define FRNUM     0x06
#define FRBASE    0x08
#define SOFMOD    0x0C
#define PORTSC(n) (0x10 + 2 * ((n) - 1))

#define CMD_RUN     0x01
#define CMD_HCRESET 0x02
#define CMD_GRESET  0x04
#define CMD_CF      0x40
#define CMD_MAXP    0x80

#define PS_CCS   0x0001
#define PS_CSC   0x0002
#define PS_PED   0x0004
#define PS_PEDC  0x0008
#define PS_LSDA  0x0100
#define PS_RESET 0x0200
#define PS_ONE   0x0080

#define LP_T   1u               /* link pointer: terminate */
#define LP_QH  2u               /* link pointer: points at a queue head */
#define LP_VF  4u               /* depth first */

#define TD_ACTIVE  (1u << 23)
#define TD_ERRMASK (0x7Eu << 16) /* stalled, buffer error, babble, nak?, crc, bitstuff */
#define TD_ERR     (0x76u << 16) /* everything except NAK (bit 19) */
#define TD_LS      (1u << 26)
#define TD_CERR3   (3u << 27)
#define TD_SPD     (1u << 29)

#define PID_SETUP 0x2D
#define PID_IN    0x69
#define PID_OUT   0xE1

#define MAX_INTR  16
#define MAX_TDS   256

struct td { volatile uint32_t link, status, token, buf; };
struct qh { volatile uint32_t head, element, pad[2]; };

struct uhci_ep {
    bool used;
    int qi;                     /* index of its interrupt QH */
    struct td *td;
    paddr_t td_p;
    int toggle, errors;
    struct usb_ep *ue;
    struct usb_dev *ud;
};

struct uhci {
    uint16_t io;
    uint32_t *frames;
    struct qh *qi;              /* MAX_INTR interrupt QHs followed by the control QH */
    paddr_t qi_p;
    struct td *tds;             /* control transfer TDs */
    paddr_t tds_p;
    struct td *itd;             /* one TD per interrupt endpoint */
    paddr_t itd_p;
    struct uhci_ep ep[MAX_INTR];
    uint8_t addr_used[16];      /* bitmap of USB addresses 1..127 */
    int nports;
};

static struct uhci U[6];
static int nuhci;

static uint16_t r16(struct uhci *u, int off) { return inw((uint16_t)(u->io + off)); }
static void w16(struct uhci *u, int off, uint16_t v) { outw((uint16_t)(u->io + off), v); }

/* ------------------------------------------------------------------ */
/* Transfers                                                           */
/* ------------------------------------------------------------------ */

static inline void barrier(void) { __asm__ volatile("" ::: "memory"); }

static uint32_t make_token(int pid, int addr, int ep, int toggle, int len)
{
    return (uint32_t)pid | ((uint32_t)addr << 8) | ((uint32_t)ep << 15) | ((uint32_t)toggle << 19) |
           ((uint32_t)(len ? len - 1 : 0x7FF) << 21);
}

static int uhci_ctrl(struct usb_dev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len)
{
    struct uhci *u = d->hc->priv;
    struct qh *qc = &u->qi[MAX_INTR];
    bool in = (rt & 0x80) != 0;
    int mps = d->mps0 ? d->mps0 : 8;
    uint32_t ls = d->speed == USB_LOW ? TD_LS : 0;
    uint8_t *setup = d->buf + 2048;                        /* the setup packet lives in the upper half of the page */
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

    struct td *t = &u->tds[n++];
    t->status = TD_ACTIVE | TD_CERR3 | ls;
    t->token = make_token(PID_SETUP, d->addr, 0, 0, 8);
    t->buf = (uint32_t)(paddr_t)setup;
    int toggle = 1;
    for (int off = 0; off < len; off += mps) {
        int chunk = len - off < mps ? len - off : mps;
        if (n >= MAX_TDS - 2)
            return -1;
        t = &u->tds[n++];
        t->status = TD_ACTIVE | TD_CERR3 | ls;
        t->token = make_token(in ? PID_IN : PID_OUT, d->addr, 0, toggle, chunk);
        t->buf = (uint32_t)(paddr_t)(d->buf + off);
        toggle ^= 1;
        ndata++;
    }
    t = &u->tds[n++];                                       /* status stage: always DATA1, opposite direction */
    t->status = TD_ACTIVE | TD_CERR3 | ls;
    t->token = make_token((len && in) ? PID_OUT : PID_IN, d->addr, 0, 1, 0);
    t->buf = 0;
    for (int i = 0; i < n; i++)
        u->tds[i].link = i + 1 < n ? ((uint32_t)(paddr_t)&u->tds[i + 1] | LP_VF) : LP_T;
    barrier();
    qc->element = (uint32_t)(paddr_t)&u->tds[0];

    uint32_t start = timer_ticks();
    bool skipped = false;
    int rc = -1;
    for (int spin = 0;; spin++) {
        bool failed = false, done = true;
        for (int i = 0; i < n; i++) {
            uint32_t st = u->tds[i].status;
            if (st & TD_ACTIVE) {
                done = false;
                break;
            }
            if (st & TD_ERR) {
                failed = true;
                break;
            }
            /* a short IN packet ends the data stage: jump to the status stage */
            if (in && i >= 1 && i <= ndata && i < n - 2 && !skipped) {
                int got = (int)((st + 1) & 0x7FF);
                int want = (int)((u->tds[i].token >> 21) + 1) & 0x7FF;
                if (got < want) {
                    skipped = true;
                    qc->element = (uint32_t)(paddr_t)&u->tds[n - 1];
                    for (int k = i + 1; k < n - 1; k++)
                        u->tds[k].status &= ~TD_ACTIVE;
                }
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
    if (rc < 0) {
        qc->element = LP_T;
        timer_sleep_ms(10);
    }
    return rc;
}

static int uhci_dev_alloc(struct usb_dev *d)
{
    d->addr = 0;
    d->mps0 = 8;
    return 0;
}

static int uhci_dev_address(struct usb_dev *d)
{
    struct uhci *u = d->hc->priv;
    int a;

    for (a = 1; a < 128; a++)
        if (!(u->addr_used[a >> 3] & (1u << (a & 7))))
            break;
    if (a >= 128)
        return -1;
    if (uhci_ctrl(d, 0x00, 5, (uint16_t)a, 0, 0) < 0)
        return -1;
    timer_sleep_ms(10);                                     /* SET_ADDRESS recovery time */
    d->addr = a;
    u->addr_used[a >> 3] |= (uint8_t)(1u << (a & 7));
    return 0;
}

static void uhci_dev_free(struct usb_dev *d)
{
    struct uhci *u = d->hc->priv;

    if (d->addr)
        u->addr_used[d->addr >> 3] &= (uint8_t)~(1u << (d->addr & 7));
}

static void arm(struct uhci *u, struct uhci_ep *e)
{
    struct usb_dev *d = e->ud;

    e->td->link = LP_T;
    e->td->buf = (uint32_t)(paddr_t)e->ue->buf;
    e->td->token = make_token(PID_IN, d->addr, e->ue->num, e->toggle, e->ue->mps);
    barrier();
    e->td->status = TD_ACTIVE | TD_CERR3 | (d->speed == USB_LOW ? TD_LS : 0);
    barrier();
    u->qi[e->qi].element = (uint32_t)e->td_p;
}

static int uhci_intr_start(struct usb_dev *d, struct usb_ep *ue)
{
    struct uhci *u = d->hc->priv;

    for (int i = 0; i < MAX_INTR; i++) {
        struct uhci_ep *e = &u->ep[i];
        if (e->used)
            continue;
        memset(e, 0, sizeof *e);
        e->used = true;
        e->qi = i;
        e->td = &u->itd[i];
        e->td_p = (paddr_t)e->td;
        e->ue = ue;
        e->ud = d;
        ue->hcpriv = e;
        arm(u, e);
        return 0;
    }
    return -1;
}

static void uhci_intr_stop(struct usb_dev *d, struct usb_ep *ue)
{
    struct uhci *u = d->hc->priv;
    struct uhci_ep *e = ue->hcpriv;

    if (!e)
        return;
    u->qi[e->qi].element = LP_T;
    timer_sleep_ms(10);                                     /* let the controller finish with the TD */
    e->used = false;
    ue->hcpriv = NULL;
}

static void uhci_poll(struct usb_hc *hc)
{
    struct uhci *u = hc->priv;

    for (int i = 0; i < MAX_INTR; i++) {
        struct uhci_ep *e = &u->ep[i];
        if (!e->used)
            continue;
        uint32_t st = e->td->status;
        if (st & TD_ACTIVE)
            continue;
        if (st & TD_ERR) {
            if (++e->errors >= 8) {
                log_warn("usb", "UHCI: endpoint %d keeps failing, stopped", e->ue->num);
                u->qi[e->qi].element = LP_T;
                e->used = false;
                continue;
            }
        } else {
            int got = (int)((st + 1) & 0x7FF);
            e->errors = 0;
            e->toggle ^= 1;
            if (got > 0)
                usb_report(e->ud, e->ue, e->ue->buf, got);
        }
        arm(u, e);
    }
}

static int uhci_rh_connected(struct usb_hc *hc, int port)
{
    struct uhci *u = hc->priv;
    uint16_t v = r16(u, PORTSC(port));

    if (v & (PS_CSC | PS_PEDC))
        w16(u, PORTSC(port), (uint16_t)((v & 0x1FF5) | (v & (PS_CSC | PS_PEDC))));
    return (v & PS_CCS) ? 1 : 0;
}

static int uhci_rh_reset(struct usb_hc *hc, int port)
{
    struct uhci *u = hc->priv;

    w16(u, PORTSC(port), PS_RESET);
    timer_sleep_ms(50);
    w16(u, PORTSC(port), 0);
    timer_sleep_ms(10);
    uint16_t v = 0;
    for (int i = 0; i < 5; i++) {
        w16(u, PORTSC(port), PS_PED | PS_CSC | PS_PEDC);
        timer_sleep_ms(10);
        v = r16(u, PORTSC(port));
        if ((v & PS_PED) && (v & PS_CCS))
            break;
    }
    if (!((v & PS_PED) && (v & PS_CCS)))
        return -1;
    return (v & PS_LSDA) ? USB_LOW : USB_FULL;
}

static const struct usb_hc_ops uhci_ops = {
    .name = "UHCI",
    .addr_first = false,
    .dev_alloc = uhci_dev_alloc,
    .dev_address = uhci_dev_address,
    .dev_free = uhci_dev_free,
    .ctrl = uhci_ctrl,
    .intr_start = uhci_intr_start,
    .intr_stop = uhci_intr_stop,
    .poll = uhci_poll,
    .rh_connected = uhci_rh_connected,
    .rh_reset = uhci_rh_reset,
};

bool uhci_probe(const struct pci_device *pd)
{
    if (nuhci >= 6)
        return false;
    uint32_t bar = pci_cfg_read(pd->bus, pd->slot, pd->func, 0x20);
    if (!(bar & 1)) {
        log_warn("usb", "UHCI: BAR4 is not an I/O range");
        return false;
    }
    struct uhci *u = &U[nuhci];
    memset(u, 0, sizeof *u);
    u->io = (uint16_t)(bar & 0xFFFC);
    usb_pci_enable(pd, true);
    /* turn off the BIOS "legacy support" (it would emulate a PS/2 keyboard from USB) */
    uint32_t leg = pci_cfg_read(pd->bus, pd->slot, pd->func, 0xC0);
    pci_cfg_write(pd->bus, pd->slot, pd->func, 0xC0, (leg & 0xFFFF0000u) | 0x8F00u);

    w16(u, USBCMD, CMD_GRESET);                             /* global reset */
    timer_sleep_ms(50);
    w16(u, USBCMD, 0);
    timer_sleep_ms(10);
    w16(u, USBCMD, CMD_HCRESET);
    for (int i = 0; i < 20 && (r16(u, USBCMD) & CMD_HCRESET); i++)
        timer_sleep_ms(10);
    if (r16(u, USBCMD) & CMD_HCRESET) {
        log_warn("usb", "UHCI: reset did not finish");
        return false;
    }
    w16(u, USBINTR, 0);
    w16(u, USBSTS, 0x3F);

    paddr_t fp, qp, tp;
    u->frames = usb_dma_pages(1, &fp);
    u->qi = usb_dma_pages(1, &qp);
    u->tds = usb_dma_pages(1, &tp);
    if (!u->frames || !u->qi || !u->tds)
        return false;
    u->qi_p = qp;
    u->tds_p = tp;
    u->itd = (struct td *)((uint8_t *)u->qi + 1024);
    u->itd_p = qp + 1024;
    for (int i = 0; i < MAX_INTR; i++) {
        u->qi[i].head = (uint32_t)(qp + (paddr_t)(i + 1) * sizeof(struct qh)) | LP_QH;
        u->qi[i].element = LP_T;
    }
    u->qi[MAX_INTR].head = LP_T;
    u->qi[MAX_INTR].element = LP_T;
    for (int i = 0; i < 1024; i++)
        u->frames[i] = (uint32_t)qp | LP_QH;

    w16(u, FRNUM, 0);
    outl((uint16_t)(u->io + FRBASE), (uint32_t)fp);
    outb((uint16_t)(u->io + SOFMOD), 0x40);
    w16(u, USBCMD, CMD_RUN | CMD_CF | CMD_MAXP);
    timer_sleep_ms(10);
    if (r16(u, USBSTS) & 0x20) {                            /* HCHalted */
        log_warn("usb", "UHCI: controller did not start");
        return false;
    }
    int np = 0;
    while (np < 8) {
        uint16_t v = r16(u, PORTSC(np + 1));
        if (v == 0xFFFF || !(v & PS_ONE))
            break;
        np++;
    }
    u->nports = np;
    log_info("usb", "UHCI %04x:%04x ready: %d ports, I/O %04x", pd->vendor, pd->device, np, u->io);
    if (usb_hc_add(&uhci_ops, "UHCI", np, u) < 0)
        return false;
    nuhci++;
    return true;
}
