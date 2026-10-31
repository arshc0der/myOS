/*
 * xhci.c - USB 3 (xHCI) host controller driver.
 *
 * How it works, in short:
 *   - The controller and the driver talk through "rings" in RAM: a command
 *     ring (driver -> controller), one transfer ring per endpoint, and one
 *     event ring (controller -> driver). Every entry is a 16-byte TRB.
 *   - A "cycle bit" tells each side whether an entry is new.
 *   - We never use interrupts: the "usb" task polls the event ring.
 *   - Each connected device gets a "slot". We reset its port, give it an
 *     address, read its descriptors, pick the HID interfaces (keyboard,
 *     mouse, tablet) and keep one interrupt-IN transfer queued on each.
 *
 * Not supported: USB hubs, EHCI/OHCI/UHCI controllers, mass storage.
 */
#include "usb.h"
#include "hw.h"
#include "io.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "timer.h"
#include "kstring.h"
#include "kprintf.h"
#include "log.h"
#include "multiboot.h"

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

#define MAX_DEVS   8
#define MAX_EPS    3
#define RING_TRBS  64

struct trb { uint32_t p0, p1, status, ctrl; };

struct ring {
    volatile struct trb *t;
    paddr_t phys;
    int n, enq, pcs;
};

struct xep {
    bool used;
    int dci, mps;
    uint8_t iface;
    struct ring ring;
    uint8_t *buf;
    int errors;
    struct hid_dev hid;
};

struct xdev {
    bool used;
    int port, slot, speed;
    uint16_t vid, pid;
    uint8_t cls;
    uint8_t *out, *in, *buf;            /* device ctx, input ctx, scratch page */
    struct ring ep0;
    struct xep ep[MAX_EPS];
};

static struct {
    bool ok;
    volatile uint8_t *cap, *op, *rt, *db;
    int ctxsz, maxslots, maxports;
    uint8_t bus, slot, func;
    uint16_t vendor, device;
    uint32_t *dcbaa;
    struct ring cmd;
    volatile struct trb *evt;
    paddr_t evt_phys;
    int evt_idx, evt_ccs;
    uint32_t pagesize;
} X;

static struct xdev devs[MAX_DEVS];
static bool port_dirty;

/* results of the command / control transfer we are waiting for */
static struct { volatile bool done; uint32_t code, slot, p0; } cmd_res;
static struct { volatile bool done; uint32_t code, slot; } ctl_res;

static inline uint32_t rd(volatile void *p) { return *(volatile uint32_t *)p; }
static inline void wr(volatile void *p, uint32_t v) { *(volatile uint32_t *)p = v; }
static inline void wr64(volatile void *p, paddr_t v) { wr(p, v); wr((volatile uint8_t *)p + 4, 0); }
static inline void barrier(void) { __asm__ volatile("" ::: "memory"); }

/* ------------------------------------------------------------------ */
/* Memory helpers                                                      */
/* ------------------------------------------------------------------ */

static void *dma_page(paddr_t *phys)
{
    paddr_t p = pmm_alloc_page();

    if (!p)
        return NULL;
    memset((void *)p, 0, 4096);
    if (phys)
        *phys = p;
    return (void *)p;
}

static void dma_free(void *p)
{
    if (p)
        pmm_free_page((paddr_t)p);
}

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

static struct xep *find_ep(int slot, int dci)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        if (!devs[i].used || devs[i].slot != slot)
            continue;
        for (int e = 0; e < MAX_EPS; e++)
            if (devs[i].ep[e].used && devs[i].ep[e].dci == dci)
                return &devs[i].ep[e];
    }
    return NULL;
}

static struct xdev *find_dev(int slot)
{
    for (int i = 0; i < MAX_DEVS; i++)
        if (devs[i].used && devs[i].slot == slot)
            return &devs[i];
    return NULL;
}

static void requeue(struct xdev *d, struct xep *e)
{
    ring_push(&e->ring, (uint32_t)(paddr_t)e->buf, 0, (uint32_t)e->mps, (TRB_NORMAL << 10) | (1 << 5) | (1 << 2));
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
    } else if (type == EV_PORT_CHG) {
        port_dirty = true;
    } else if (type == EV_TRANSFER) {
        int slot = (int)(ctrl >> 24), dci = (int)((ctrl >> 16) & 31);
        if (dci == 1) {
            ctl_res.code = code;
            ctl_res.slot = (uint32_t)slot;
            ctl_res.done = true;
            return;
        }
        struct xdev *d = find_dev(slot);
        struct xep *e = find_ep(slot, dci);
        if (!d || !e)
            return;
        if (code == CC_SUCCESS || code == CC_SHORT) {
            int got = e->mps - (int)(status & 0xFFFFFF);
            if (got > 0)
                hid_report(&e->hid, e->buf, got);
            e->errors = 0;
            requeue(d, e);
        } else if (++e->errors < 8) {
            log_warn("usb", "slot %d endpoint %d: transfer error %u", slot, dci, code);
            requeue(d, e);
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

/* Control transfer on endpoint 0. Data goes through d->buf. Returns 0 or -1. */
static int ctrl_xfer(struct xdev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
                     uint16_t len, bool in)
{
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;

    ctl_res.done = false;
    ring_push(&d->ep0, (uint32_t)rt | ((uint32_t)req << 8) | ((uint32_t)val << 16),
              (uint32_t)idx | ((uint32_t)len << 16), 8, (TRB_SETUP << 10) | (1 << 6) | (trt << 16));
    if (len)
        ring_push(&d->ep0, (uint32_t)(paddr_t)d->buf, 0, len, (TRB_DATA << 10) | (in ? (1u << 16) : 0));
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

static int get_descriptor(struct xdev *d, uint8_t type, uint8_t index, uint16_t len)
{
    return ctrl_xfer(d, 0x80, 6, (uint16_t)((type << 8) | index), 0, len, true);
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

static int default_mps(int speed)
{
    switch (speed) {
    case 1: return 64;      /* full speed: corrected after the first 8 bytes */
    case 2: return 8;       /* low speed  */
    case 3: return 64;      /* high speed */
    default: return 512;    /* super speed */
    }
}

static const char *speed_name(int s)
{
    switch (s) {
    case 1: return "full";
    case 2: return "low";
    case 3: return "high";
    case 4: return "super";
    default: return "?";
    }
}

static void free_dev(struct xdev *d)
{
    for (int e = 0; e < MAX_EPS; e++)
        if (d->ep[e].used) {
            ring_free(&d->ep[e].ring);
            dma_free(d->ep[e].buf);
        }
    ring_free(&d->ep0);
    dma_free(d->out);
    dma_free(d->in);
    dma_free(d->buf);
    memset(d, 0, sizeof *d);
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                         */
/* ------------------------------------------------------------------ */

static volatile uint32_t *portsc(int port) { return (volatile uint32_t *)(X.op + 0x400 + 0x10 * (port - 1)); }

#define PORT_CCS  (1u << 0)
#define PORT_PED  (1u << 1)
#define PORT_PR   (1u << 4)
#define PORT_PP   (1u << 9)
#define PORT_PRC  (1u << 21)
#define PORT_RW1C 0x00FE0002u           /* change bits + PED: write 1 clears/disables */

static bool port_reset(int port)
{
    uint32_t v = rd(portsc(port));

    if ((v & PORT_PED) && ((v >> 10) & 15) == 4)         /* super speed ports train themselves */
        return true;
    wr(portsc(port), (v & ~PORT_RW1C) | PORT_PR);
    uint32_t start = timer_ticks();
    while (!(rd(portsc(port)) & PORT_PRC)) {
        if (timer_ticks() - start > 100)
            return false;
        timer_sleep_ms(10);
    }
    v = rd(portsc(port));
    wr(portsc(port), (v & ~PORT_RW1C) | PORT_PRC);       /* acknowledge */
    timer_sleep_ms(20);                                  /* reset recovery time */
    return (rd(portsc(port)) & (PORT_PED | PORT_CCS)) == (PORT_PED | PORT_CCS);
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

static void parse_config(struct xdev *d, int total)
{
    const uint8_t *c = d->buf;
    int iface = -1, cls = 0, sub = 0, proto = 0, hidlen = 0, ne = 0;
    uint8_t cfgval = c[5];
    uint8_t ifnum[MAX_EPS], sub_[MAX_EPS], proto_[MAX_EPS];
    uint16_t hlen[MAX_EPS], mps_[MAX_EPS];
    uint8_t epaddr[MAX_EPS], interval[MAX_EPS];
    bool have_ep[MAX_EPS] = { false };

    for (int i = 0; i + 2 <= total && c[i] >= 2;) {
        int len = c[i], type = c[i + 1];
        if (type == 4 && len >= 9) {                      /* interface */
            iface = c[i + 2];
            cls = c[i + 5]; sub = c[i + 6]; proto = c[i + 7];
            hidlen = 0;
            if (c[i + 3] == 0 && cls == 3 && ne < MAX_EPS) {
                ifnum[ne] = (uint8_t)iface;
                sub_[ne] = (uint8_t)sub;
                proto_[ne] = (uint8_t)proto;
                hlen[ne] = 0;
                have_ep[ne] = false;
                ne++;
            } else if (c[i + 3] != 0) {
                iface = -1;                               /* alternate setting: ignore */
            } else {
                iface = -1;
                if (d->cls == 0)
                    d->cls = (uint8_t)cls;
            }
        } else if (type == 0x21 && len >= 9 && iface >= 0 && cls == 3 && ne > 0) {
            hidlen = c[i + 7] | (c[i + 8] << 8);
            hlen[ne - 1] = (uint16_t)hidlen;
        } else if (type == 5 && len >= 7 && iface >= 0 && cls == 3 && ne > 0) {
            int k = ne - 1;
            if ((c[i + 3] & 3) == 3 && (c[i + 2] & 0x80) && !have_ep[k]) {
                have_ep[k] = true;
                epaddr[k] = c[i + 2];
                mps_[k] = (uint16_t)((c[i + 4] | (c[i + 5] << 8)) & 0x7FF);
                interval[k] = c[i + 6];
            }
        }
        i += len;
    }

    int found = 0;
    for (int k = 0; k < ne; k++)
        if (have_ep[k])
            found++;
    if (!found) {
        log_info("usb", "port %d: %04x:%04x - no HID interface (class %u), ignored", d->port, d->vid, d->pid, d->cls);
        return;
    }

    if (ctrl_xfer(d, 0x00, 9, cfgval, 0, 0, false) < 0) {      /* SET_CONFIGURATION */
        log_warn("usb", "port %d: SET_CONFIGURATION failed", d->port);
        return;
    }

    int n = 0, maxdci = 1;
    for (int k = 0; k < ne && n < MAX_EPS; k++) {
        if (!have_ep[k])
            continue;
        struct xep *e = &d->ep[n];
        int kind = HID_NONE;
        e->iface = ifnum[k];
        e->dci = (epaddr[k] & 15) * 2 + 1;
        e->mps = mps_[k] > 64 && d->speed < 4 ? 64 : (mps_[k] ? mps_[k] : 8);
        if (e->mps > 64)
            e->mps = 64;                                     /* our report buffer is small */

        if (sub_[k] == 1) {                                  /* boot interface */
            kind = proto_[k] == 1 ? HID_KEYBOARD : (proto_[k] == 2 ? HID_MOUSE : HID_NONE);
            ctrl_xfer(d, 0x21, 0x0B, 0, e->iface, 0, false);     /* SET_PROTOCOL = boot */
            hid_setup(&e->hid, kind, NULL, 0);
        } else {
            int rl = hlen[k] > 0 && hlen[k] <= 1024 ? hlen[k] : 256;
            if (rl > 4096) rl = 4096;
            if (ctrl_xfer(d, 0x81, 6, 0x2200, e->iface, (uint16_t)rl, true) == 0) {
                hid_setup(&e->hid, HID_NONE, d->buf, rl);
                kind = e->hid.kind;
            }
        }
        if (kind == HID_NONE) {
            log_info("usb", "port %d: interface %d is not a keyboard or pointer", d->port, e->iface);
            continue;
        }
        if (kind == HID_KEYBOARD)
            ctrl_xfer(d, 0x21, 0x0A, 0, e->iface, 0, false);     /* SET_IDLE: only report changes */

        if (!ring_alloc(&e->ring, RING_TRBS))
            continue;
        e->buf = dma_page(NULL);
        if (!e->buf) {
            ring_free(&e->ring);
            continue;
        }
        e->used = true;
        int intv;
        if (d->speed >= 3) {
            intv = interval[k] - 1;
            if (intv < 0) intv = 0;
            if (intv > 15) intv = 15;
        } else {
            intv = ilog2(interval[k] ? interval[k] : 8) + 3;
            if (intv < 3) intv = 3;
            if (intv > 10) intv = 10;
        }
        ep_context(d->in, e->dci, 7, e->mps, intv, e->ring.phys);
        if (e->dci > maxdci)
            maxdci = e->dci;
        n++;
        log_info("usb", "port %d: %s on interface %d (%s speed, endpoint %d)", d->port,
                 kind == HID_KEYBOARD ? "keyboard" : (kind == HID_MOUSE ? "mouse/touchpad" : "tablet/pointer"),
                 e->iface, speed_name(d->speed), e->dci / 2);
    }
    if (!n)
        return;

    /* tell the controller about the new endpoints */
    uint32_t *ctl = ctx(d->in, 0);
    ctl[0] = 0;
    ctl[1] = 1;                                              /* A0: slot context */
    for (int e = 0; e < MAX_EPS; e++)
        if (d->ep[e].used)
            ctl[1] |= 1u << d->ep[e].dci;
    uint32_t *sl = ctx(d->in, 1);
    sl[0] = ((uint32_t)maxdci << 27) | ((uint32_t)d->speed << 20);
    sl[1] = (uint32_t)d->port << 16;
    int cc = command((uint32_t)(paddr_t)d->in, (CMD_CONFIG_EP << 10) | ((uint32_t)d->slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        log_warn("usb", "port %d: configure endpoint failed (code %d)", d->port, cc);
        for (int e = 0; e < MAX_EPS; e++)
            d->ep[e].used = false;
        return;
    }
    for (int e = 0; e < MAX_EPS; e++)
        if (d->ep[e].used)
            requeue(d, &d->ep[e]);
}

static void enumerate_port(int port)
{
    if (!port_reset(port)) {
        log_warn("usb", "port %d: reset failed", port);
        return;
    }
    int speed = (int)((rd(portsc(port)) >> 10) & 15);

    uint32_t slot = 0;
    int cc = command(0, CMD_ENABLE_SLOT << 10, &slot);
    if (cc != CC_SUCCESS || slot == 0 || slot > (uint32_t)X.maxslots) {
        log_warn("usb", "port %d: no free device slot (code %d)", port, cc);
        return;
    }
    struct xdev *d = NULL;
    for (int i = 0; i < MAX_DEVS; i++)
        if (!devs[i].used) {
            d = &devs[i];
            break;
        }
    if (!d) {
        log_warn("usb", "too many USB devices");
        return;
    }
    memset(d, 0, sizeof *d);
    paddr_t out_p = 0, in_p = 0;
    d->out = dma_page(&out_p);
    d->in = dma_page(&in_p);
    d->buf = dma_page(NULL);
    if (!d->out || !d->in || !d->buf || !ring_alloc(&d->ep0, RING_TRBS)) {
        free_dev(d);
        return;
    }
    d->used = true;
    d->port = port;
    d->slot = (int)slot;
    d->speed = speed;
    X.dcbaa[slot * 2] = out_p;
    X.dcbaa[slot * 2 + 1] = 0;

    int mps = default_mps(speed);
    uint32_t *ctl = ctx(d->in, 0);
    ctl[1] = 3;                                              /* A0 + A1 */
    uint32_t *sl = ctx(d->in, 1);
    sl[0] = (1u << 27) | ((uint32_t)speed << 20);
    sl[1] = (uint32_t)port << 16;
    uint32_t *e0 = ctx(d->in, 2);
    e0[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16);
    e0[2] = d->ep0.phys | 1;
    e0[4] = 8;
    cc = command(in_p, (CMD_ADDRESS_DEV << 10) | (slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        log_warn("usb", "port %d: address device failed (code %d)", port, cc);
        goto fail;
    }

    /* first 8 bytes tell us the real packet size of endpoint 0 */
    if (get_descriptor(d, 1, 0, 8) < 0)
        goto fail;
    if (speed <= 2) {
        int real = d->buf[7];
        if (real >= 8 && real != mps) {
            memset(d->in, 0, 4096);
            ctl[1] = 2;                                      /* A1: endpoint 0 only */
            e0[1] = (3u << 1) | (4u << 3) | ((uint32_t)real << 16);
            e0[2] = d->ep0.phys | 1;
            command(in_p, (CMD_EVAL_CTX << 10) | (slot << 24), NULL);
        }
    }
    if (get_descriptor(d, 1, 0, 18) < 0)
        goto fail;
    d->vid = (uint16_t)(d->buf[8] | (d->buf[9] << 8));
    d->pid = (uint16_t)(d->buf[10] | (d->buf[11] << 8));
    d->cls = d->buf[4];
    if (d->cls == 9) {
        log_info("usb", "port %d: %04x:%04x is a USB hub (hubs are not supported)", port, d->vid, d->pid);
        return;
    }

    if (get_descriptor(d, 2, 0, 9) < 0)
        goto fail;
    int total = d->buf[2] | (d->buf[3] << 8);
    if (total > 1024)
        total = 1024;
    if (total < 9)
        goto fail;
    if (get_descriptor(d, 2, 0, (uint16_t)total) < 0)
        goto fail;
    memset(d->in, 0, 4096);
    parse_config(d, total);
    return;
fail:
    log_warn("usb", "port %d: could not read the device descriptors", port);
    command(0, (CMD_DISABLE_SLOT << 10) | (slot << 24), NULL);
    X.dcbaa[slot * 2] = 0;
    free_dev(d);
}

static void remove_port(int port)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        if (devs[i].used && devs[i].port == port) {
            int slot = devs[i].slot;
            log_info("usb", "port %d: device removed", port);
            command(0, (CMD_DISABLE_SLOT << 10) | ((uint32_t)slot << 24), NULL);
            X.dcbaa[slot * 2] = 0;
            free_dev(&devs[i]);
        }
    }
}

static uint8_t tried[64];

static void scan_ports(void)
{
    for (int p = 1; p <= X.maxports; p++) {
        uint32_t v = rd(portsc(p));
        bool known = false;
        for (int i = 0; i < MAX_DEVS; i++)
            if (devs[i].used && devs[i].port == p)
                known = true;
        if (v & 0x00FE0000u)                                 /* acknowledge change bits */
            wr(portsc(p), (v & ~PORT_RW1C) | (v & 0x00FE0000u & ~PORT_PRC));
        if (!(v & PORT_CCS)) {
            tried[p & 63] = 0;
            if (known)
                remove_port(p);
        } else if (!known && !tried[p & 63]) {
            tried[p & 63] = 1;                               /* try once per plug-in */
            enumerate_port(p);
        }
    }
}

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

static void usb_task(void *arg)
{
    (void)arg;
    static struct pci_device list[64];
    int n = pci_scan(list, 64);
    const struct pci_device *pd = NULL;

    if (n > 64)
        n = 64;
    for (int i = 0; i < n && !pd; i++)
        if (list[i].cls == 0x0C && list[i].sub == 0x03 &&
            ((pci_cfg_read(list[i].bus, list[i].slot, list[i].func, 8) >> 8) & 0xFF) == 0x30)
            pd = &list[i];
    if (!pd) {
        log_info("usb", "no xHCI (USB 3) controller found");
        task_exit(0);
    }
    X.bus = pd->bus; X.slot = pd->slot; X.func = pd->func;
    X.vendor = pd->vendor; X.device = pd->device;
    if (!controller_init(pd)) {
        log_warn("usb", "xHCI controller %04x:%04x could not be started", pd->vendor, pd->device);
        task_exit(1);
    }
    log_info("usb", "xHCI %04x:%04x ready: %d ports, %d slots, %d-byte contexts",
             pd->vendor, pd->device, X.maxports, X.maxslots, X.ctxsz);
    scan_ports();

    uint32_t last_scan = timer_ticks();
    for (;;) {
        events_poll();
        uint32_t now = timer_ticks();
        for (int i = 0; i < MAX_DEVS; i++)
            if (devs[i].used)
                for (int e = 0; e < MAX_EPS; e++)
                    if (devs[i].ep[e].used)
                        hid_poll(&devs[i].ep[e].hid, now);
        if (port_dirty || now - last_scan > 100) {
            port_dirty = false;
            last_scan = now;
            scan_ports();
        }
        timer_sleep_ms(5);
    }
}

void usb_start(void)
{
    if (boot_has_arg("nousb"))
        return;
    task_create("usb", usb_task, NULL);
}

void usb_dump(void)
{
    if (!X.ok) {
        kprintf("No USB controller running (xHCI only; boot option \"nousb\" disables it).\n");
        return;
    }
    kprintf("xHCI %04x:%04x at %02x:%02x.%x  ports %d  slots %d\n", X.vendor, X.device,
            X.bus, X.slot, X.func, X.maxports, X.maxslots);
    int cnt = 0;
    for (int i = 0; i < MAX_DEVS; i++) {
        if (!devs[i].used)
            continue;
        cnt++;
        kprintf("  port %d  %04x:%04x  %s speed  ", devs[i].port, devs[i].vid, devs[i].pid,
                speed_name(devs[i].speed));
        bool any = false;
        for (int e = 0; e < MAX_EPS; e++) {
            if (!devs[i].ep[e].used)
                continue;
            int k = devs[i].ep[e].hid.kind;
            kprintf("%s%s", any ? ", " : "", k == HID_KEYBOARD ? "keyboard" :
                    k == HID_MOUSE ? "mouse" : "tablet");
            any = true;
        }
        kprintf("%s\n", any ? "" : "(no supported interface)");
    }
    if (!cnt)
        kprintf("  no devices\n");
}
