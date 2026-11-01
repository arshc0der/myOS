/*
 * usbcore.c - USB enumeration, hubs, HID setup, hot plug and the "usb" task.
 * See usbcore.h for how this fits together.
 */
#include "usbcore.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "timer.h"
#include "kstring.h"
#include "kprintf.h"
#include "log.h"
#include "multiboot.h"

static struct usb_hc hcs[USB_MAX_HC];
static int nhcs;
static struct usb_dev devs[USB_MAX_DEVS];

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

void *usb_dma_pages(int pages, paddr_t *phys)
{
    paddr_t p = pages == 1 ? pmm_alloc_page() : pmm_alloc_contiguous((uint32_t)pages);

    if (!p)
        return NULL;
    memset((void *)p, 0, (size_t)pages * 4096);
    if (phys)
        *phys = p;
    return (void *)p;
}

void usb_dma_free(void *p, int pages)
{
    if (p)
        pmm_free_pages((paddr_t)p, (uint32_t)pages);
}

void usb_pci_enable(const struct pci_device *pd, bool io)
{
    uint32_t cmd = pci_cfg_read(pd->bus, pd->slot, pd->func, 0x04);

    pci_cfg_write(pd->bus, pd->slot, pd->func, 0x04, (cmd & 0xFFFF) | (io ? 1u : 2u) | 4u);
}

volatile uint8_t *usb_map_bar0(const struct pci_device *pd)
{
    uint32_t bar = pci_cfg_read(pd->bus, pd->slot, pd->func, 0x10);

    if (bar & 1) {
        log_warn("usb", "controller BAR0 is an I/O range (unexpected)");
        return NULL;
    }
    if (((bar >> 1) & 3) == 2 && pci_cfg_read(pd->bus, pd->slot, pd->func, 0x14) != 0) {
        log_warn("usb", "controller registers are above 4 GiB (unsupported)");
        return NULL;
    }
    uint32_t base = bar & ~0xFu;
    if (!base) {
        log_warn("usb", "controller has no memory address assigned");
        return NULL;
    }
    uint32_t page = base & ~0xFFFu;
    usb_pci_enable(pd, false);
    if (vmm_map_range(page, page, 0x2000, VMM_WRITE | VMM_NOCACHE) < 0) {
        log_warn("usb", "cannot map the controller registers");
        return NULL;
    }
    return (volatile uint8_t *)base;
}

int usb_hc_add(const struct usb_hc_ops *ops, const char *name, int nports, void *priv)
{
    if (nhcs >= USB_MAX_HC)
        return -1;
    struct usb_hc *hc = &hcs[nhcs];

    memset(hc, 0, sizeof *hc);
    hc->ops = ops;
    strlcpy(hc->name, name, sizeof hc->name);
    hc->nports = nports > 31 ? 31 : nports;
    hc->priv = priv;
    return nhcs++;
}

void usb_report(struct usb_dev *d, struct usb_ep *e, const uint8_t *data, int len)
{
    (void)d;
    hid_report(&e->hid, data, len);
}

static const char *speed_name(int s)
{
    switch (s) {
    case USB_LOW: return "low";
    case USB_FULL: return "full";
    case USB_HIGH: return "high";
    case USB_SUPER: return "super";
    default: return "?";
    }
}

static int ctrl(struct usb_dev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len)
{
    return d->hc->ops->ctrl(d, rt, req, val, idx, len);
}

static int get_descriptor(struct usb_dev *d, uint8_t type, uint8_t index, uint16_t len)
{
    return ctrl(d, 0x80, 6, (uint16_t)((type << 8) | index), 0, len);
}

/* ------------------------------------------------------------------ */
/* Removal                                                             */
/* ------------------------------------------------------------------ */

static void remove_dev(struct usb_dev *d)
{
    for (int p = 1; p <= USB_MAX_HUB_PORTS; p++)
        if (d->child[p]) {
            remove_dev(d->child[p]);
            d->child[p] = NULL;
            d->child_state[p] = 0;
        }
    log_info("usb", "%s: device %04x:%04x removed", d->hc->name, d->vid, d->pid);
    for (int e = 0; e < USB_MAX_EPS; e++)
        if (d->ep[e].used) {
            if (d->hc->ops->intr_stop)
                d->hc->ops->intr_stop(d, &d->ep[e]);
            usb_dma_free(d->ep[e].buf, 1);
        }
    if (d->hc->ops->dev_free)
        d->hc->ops->dev_free(d);
    usb_dma_free(d->buf, 1);
    memset(d, 0, sizeof *d);
}

/* ------------------------------------------------------------------ */
/* Configuration: HID interfaces and hubs                              */
/* ------------------------------------------------------------------ */

static void attach_hid(struct usb_dev *d, int total)
{
    const uint8_t *c = d->buf;
    uint8_t cfgval = c[5];
    uint8_t ifnum[USB_MAX_EPS], sub[USB_MAX_EPS], proto[USB_MAX_EPS];
    uint16_t hlen[USB_MAX_EPS], mps[USB_MAX_EPS];
    uint8_t epnum[USB_MAX_EPS], interval[USB_MAX_EPS];
    bool have_ep[USB_MAX_EPS] = { false };
    int iface = -1, cls = 0, ne = 0;

    for (int i = 0; i + 2 <= total && c[i] >= 2;) {
        int len = c[i], type = c[i + 1];
        if (type == 4 && len >= 9) {
            cls = c[i + 5];
            iface = -1;
            if (c[i + 3] == 0 && cls == 3 && ne < USB_MAX_EPS) {
                iface = c[i + 2];
                ifnum[ne] = (uint8_t)iface;
                sub[ne] = c[i + 6];
                proto[ne] = c[i + 7];
                hlen[ne] = 0;
                have_ep[ne] = false;
                ne++;
            }
        } else if (type == 0x21 && len >= 9 && iface >= 0 && ne > 0) {
            hlen[ne - 1] = (uint16_t)(c[i + 7] | (c[i + 8] << 8));
        } else if (type == 5 && len >= 7 && iface >= 0 && ne > 0) {
            int k = ne - 1;
            if ((c[i + 3] & 3) == 3 && (c[i + 2] & 0x80) && !have_ep[k]) {
                have_ep[k] = true;
                epnum[k] = c[i + 2] & 15;
                mps[k] = (uint16_t)((c[i + 4] | (c[i + 5] << 8)) & 0x7FF);
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
        log_info("usb", "%s port %d: %04x:%04x has no HID interface (class %u), ignored",
                 d->hc->name, d->port, d->vid, d->pid, d->cls);
        return;
    }
    if (ctrl(d, 0x00, 9, cfgval, 0, 0) < 0) {                  /* SET_CONFIGURATION */
        log_warn("usb", "%s port %d: SET_CONFIGURATION failed", d->hc->name, d->port);
        return;
    }
    int n = 0;
    for (int k = 0; k < ne && n < USB_MAX_EPS; k++) {
        if (!have_ep[k])
            continue;
        struct usb_ep *e = &d->ep[n];
        int kind = HID_NONE;
        memset(e, 0, sizeof *e);
        e->iface = ifnum[k];
        e->num = epnum[k];
        e->mps = mps[k] ? (mps[k] > 64 ? 64 : mps[k]) : 8;
        e->interval = interval[k];
        if (sub[k] == 1) {                                     /* boot interface */
            kind = proto[k] == 1 ? HID_KEYBOARD : (proto[k] == 2 ? HID_MOUSE : HID_NONE);
            ctrl(d, 0x21, 0x0B, 0, e->iface, 0);               /* SET_PROTOCOL: boot */
            hid_setup(&e->hid, kind, NULL, 0);
        } else {
            int rl = hlen[k] > 0 && hlen[k] <= 2048 ? hlen[k] : 128;
            if (ctrl(d, 0x81, 6, 0x2200, e->iface, (uint16_t)rl) == 0) {
                hid_setup(&e->hid, HID_NONE, d->buf, rl);
                kind = e->hid.kind;
            }
        }
        if (kind == HID_NONE) {
            log_info("usb", "%s port %d: interface %d is not a keyboard or pointer", d->hc->name, d->port, e->iface);
            continue;
        }
        if (kind == HID_KEYBOARD)
            ctrl(d, 0x21, 0x0A, 0, e->iface, 0);               /* SET_IDLE */
        e->buf = usb_dma_pages(1, NULL);
        if (!e->buf)
            continue;
        e->used = true;
        n++;
        log_info("usb", "%s port %d: %s (%s speed)", d->hc->name, d->port,
                 kind == HID_KEYBOARD ? "keyboard" : (kind == HID_MOUSE ? "mouse/touchpad" : "tablet/pointer"),
                 speed_name(d->speed));
    }
    if (!n)
        return;
    if (d->hc->ops->configure && d->hc->ops->configure(d) < 0) {
        log_warn("usb", "%s port %d: could not configure the endpoints", d->hc->name, d->port);
        for (int e = 0; e < USB_MAX_EPS; e++)
            d->ep[e].used = false;
        return;
    }
    for (int e = 0; e < USB_MAX_EPS; e++)
        if (d->ep[e].used)
            d->hc->ops->intr_start(d, &d->ep[e]);
}

/* ---- hubs ---- */

#define HUB_FEAT_RESET        4
#define HUB_FEAT_POWER        8
#define HUB_FEAT_C_CONNECTION 16
#define HUB_FEAT_C_ENABLE     17
#define HUB_FEAT_C_RESET      20

static struct usb_dev *enumerate(struct usb_hc *hc, struct usb_dev *parent, int port, int speed);

static int hub_set_feature(struct usb_dev *h, int feature, int port)
{
    return ctrl(h, 0x23, 3, (uint16_t)feature, (uint16_t)port, 0);
}

static int hub_clear_feature(struct usb_dev *h, int feature, int port)
{
    return ctrl(h, 0x23, 1, (uint16_t)feature, (uint16_t)port, 0);
}

static int hub_status(struct usb_dev *h, int port, uint16_t *st, uint16_t *chg)
{
    if (ctrl(h, 0xA3, 0, 0, (uint16_t)port, 4) < 0)
        return -1;
    *st = (uint16_t)(h->buf[0] | (h->buf[1] << 8));
    *chg = (uint16_t)(h->buf[2] | (h->buf[3] << 8));
    return 0;
}

static void hub_init(struct usb_dev *h, int cfgval)
{
    if (ctrl(h, 0x00, 9, (uint16_t)cfgval, 0, 0) < 0) {
        log_warn("usb", "%s: hub SET_CONFIGURATION failed", h->hc->name);
        return;
    }
    uint16_t dt = h->speed == USB_SUPER ? 0x2A00 : 0x2900;
    if (ctrl(h, 0xA0, 6, dt, 0, 7) < 0) {
        log_warn("usb", "%s: cannot read the hub descriptor", h->hc->name);
        return;
    }
    h->nports = h->buf[2];
    int power_ms = h->buf[5] * 2;
    if (h->nports < 1)
        return;
    if (h->nports > USB_MAX_HUB_PORTS) {
        log_warn("usb", "hub has %d ports, only the first %d are used", h->nports, USB_MAX_HUB_PORTS);
        h->nports = USB_MAX_HUB_PORTS;
    }
    h->is_hub = true;
    if (h->hc->ops->configure)
        h->hc->ops->configure(h);                              /* xHCI: tell the controller it is a hub */
    for (int p = 1; p <= h->nports; p++)
        hub_set_feature(h, HUB_FEAT_POWER, p);
    timer_sleep_ms((uint32_t)(power_ms < 100 ? 100 : power_ms) + 20);
    log_info("usb", "%s: hub with %d ports", h->hc->name, h->nports);
}

static void hub_scan(struct usb_dev *h)
{
    for (int p = 1; p <= h->nports; p++) {
        uint16_t st, chg;
        if (hub_status(h, p, &st, &chg) < 0)
            continue;
        if (chg & 1)
            hub_clear_feature(h, HUB_FEAT_C_CONNECTION, p);
        if (chg & 2)
            hub_clear_feature(h, HUB_FEAT_C_ENABLE, p);
        if (!(st & 1)) {                                       /* nothing connected */
            h->child_state[p] = 0;
            if (h->child[p]) {
                remove_dev(h->child[p]);
                h->child[p] = NULL;
            }
            continue;
        }
        if (h->child_state[p])
            continue;
        h->child_state[p] = 2;                                 /* one attempt per plug-in */
        hub_set_feature(h, HUB_FEAT_RESET, p);
        uint32_t start = timer_ticks();
        for (;;) {
            timer_sleep_ms(20);
            if (hub_status(h, p, &st, &chg) < 0)
                break;
            if (chg & 0x10)                                    /* reset complete */
                break;
            if (timer_ticks() - start > 100)
                break;
        }
        hub_clear_feature(h, HUB_FEAT_C_RESET, p);
        if (!(st & 2)) {
            log_warn("usb", "%s: hub port %d did not enable", h->hc->name, p);
            continue;
        }
        int speed;
        if (h->speed == USB_SUPER)
            speed = USB_SUPER;
        else
            speed = (st & 0x200) ? USB_LOW : ((st & 0x400) ? USB_HIGH : USB_FULL);
        if (h->hc->ops->no_split && speed < USB_HIGH) {
            log_warn("usb", "%s: a %s speed device behind a hub needs split transactions (unsupported here)",
                     h->hc->name, speed_name(speed));
            continue;
        }
        timer_sleep_ms(30);
        struct usb_dev *c = enumerate(h->hc, h, p, speed);
        if (c) {
            h->child[p] = c;
            h->child_state[p] = 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                         */
/* ------------------------------------------------------------------ */

static struct usb_dev *enumerate(struct usb_hc *hc, struct usb_dev *parent, int port, int speed)
{
    struct usb_dev *d = NULL;
    int depth = parent ? parent->depth + 1 : 0;

    if (depth > 4) {
        log_warn("usb", "hubs nested too deeply");
        return NULL;
    }
    for (int i = 0; i < USB_MAX_DEVS; i++)
        if (!devs[i].used) {
            d = &devs[i];
            break;
        }
    if (!d) {
        log_warn("usb", "too many USB devices");
        return NULL;
    }
    memset(d, 0, sizeof *d);
    d->hc = hc;
    d->parent = parent;
    d->port = port;
    d->depth = depth;
    d->speed = speed;
    d->root_port = parent ? parent->root_port : port;
    d->route = parent ? (parent->route | ((uint32_t)(port & 15) << (4 * parent->depth))) : 0;
    d->mps0 = speed == USB_SUPER ? 512 : (speed == USB_HIGH ? 64 : 8);
    d->buf = usb_dma_pages(1, NULL);
    d->used = true;
    if (!d->buf || hc->ops->dev_alloc(d) < 0) {
        log_warn("usb", "%s: cannot allocate a device", hc->name);
        goto fail;
    }
    if (hc->ops->addr_first && hc->ops->dev_address(d) < 0) {
        log_warn("usb", "%s port %d: address device failed", hc->name, port);
        goto fail;
    }
    if (get_descriptor(d, 1, 0, 8) < 0)
        goto fail;
    int real = d->buf[7];
    if (speed == USB_SUPER)
        real = 1 << (real > 15 ? 9 : real);
    if (real < 8)
        real = 8;
    if (speed < USB_HIGH && real != d->mps0 && hc->ops->set_mps0) {
        d->mps0 = real;
        hc->ops->set_mps0(d, real);
    } else if (!hc->ops->addr_first) {
        d->mps0 = real;
    }
    if (!hc->ops->addr_first) {
        if (hc->ops->dev_address(d) < 0) {
            log_warn("usb", "%s port %d: SET_ADDRESS failed", hc->name, port);
            goto fail;
        }
    }
    if (get_descriptor(d, 1, 0, 18) < 0)
        goto fail;
    d->vid = (uint16_t)(d->buf[8] | (d->buf[9] << 8));
    d->pid = (uint16_t)(d->buf[10] | (d->buf[11] << 8));
    d->cls = d->buf[4];

    if (get_descriptor(d, 2, 0, 9) < 0)
        goto fail;
    int total = d->buf[2] | (d->buf[3] << 8);
    if (total < 9)
        goto fail;
    if (total > 1024)
        total = 1024;
    if (get_descriptor(d, 2, 0, (uint16_t)total) < 0)
        goto fail;
    int cfgval = d->buf[5];

    bool hub = d->cls == 9;
    if (!hub) {                                                /* hub interface in a composite device */
        for (int i = 9; i + 9 <= total && d->buf[i] >= 2; i += d->buf[i])
            if (d->buf[i + 1] == 4 && d->buf[i + 5] == 9)
                hub = true;
    }
    if (hub) {
        hub_init(d, cfgval);
        return d;
    }
    attach_hid(d, total);
    return d;
fail:
    log_warn("usb", "%s port %d: could not read the device descriptors", hc->name, port);
    if (hc->ops->dev_free)
        hc->ops->dev_free(d);
    usb_dma_free(d->buf, 1);
    memset(d, 0, sizeof *d);
    return NULL;
}

static void scan_root(struct usb_hc *hc)
{
    for (int p = 1; p <= hc->nports; p++) {
        int present = hc->ops->rh_connected(hc, p);

        if (!present) {
            hc->rh_state[p] = 0;
            if (hc->rh_dev[p]) {
                remove_dev(hc->rh_dev[p]);
                hc->rh_dev[p] = NULL;
            }
            continue;
        }
        if (hc->rh_state[p])
            continue;
        hc->rh_state[p] = 2;                                   /* one attempt per plug-in */
        int speed = hc->ops->rh_reset(hc, p);
        if (speed == -2)                                       /* handed over to a companion controller */
            continue;
        if (speed < 0) {
            log_warn("usb", "%s port %d: reset failed", hc->name, p);
            continue;
        }
        struct usb_dev *d = enumerate(hc, NULL, p, speed);
        if (d) {
            hc->rh_dev[p] = d;
            hc->rh_state[p] = 1;
        }
    }
}

static void hubs_scan_all(void)
{
    for (int i = 0; i < USB_MAX_DEVS; i++)
        if (devs[i].used && devs[i].is_hub)
            hub_scan(&devs[i]);
}

/* ------------------------------------------------------------------ */
/* The usb task                                                        */
/* ------------------------------------------------------------------ */

static void usb_task(void *arg)
{
    (void)arg;
    static struct pci_device list[96];
    int n = pci_scan(list, 96);

    if (n > 96)
        n = 96;
    /* EHCI first (it hands low/full speed ports to the companion controllers), then the rest. */
    static const uint8_t order[4] = { 0x20, 0x30, 0x10, 0x00 };
    for (int o = 0; o < 4; o++)
        for (int i = 0; i < n; i++) {
            if (list[i].cls != 0x0C || list[i].sub != 0x03)
                continue;
            uint8_t pi = (uint8_t)((pci_cfg_read(list[i].bus, list[i].slot, list[i].func, 8) >> 8) & 0xFF);
            if (pi != order[o])
                continue;
            bool ok = pi == 0x30 ? xhci_probe(&list[i]) : pi == 0x20 ? ehci_probe(&list[i]) :
                      pi == 0x10 ? ohci_probe(&list[i]) : uhci_probe(&list[i]);
            if (!ok)
                log_warn("usb", "%s controller %04x:%04x could not be started",
                         pi == 0x30 ? "xHCI" : pi == 0x20 ? "EHCI" : pi == 0x10 ? "OHCI" : "UHCI",
                         list[i].vendor, list[i].device);
        }
    if (!nhcs) {
        log_info("usb", "no USB controller found");
        task_exit(0);
    }
    timer_sleep_ms(100);
    for (int i = 0; i < nhcs; i++)
        scan_root(&hcs[i]);
    hubs_scan_all();

    uint32_t last_scan = timer_ticks(), last_hub = last_scan, started = last_scan;
    bool dumped = false;
    for (;;) {
        uint32_t now = timer_ticks();

        if (!dumped && now - started > 300 && boot_has_arg("usbdump")) {   /* used by the emulator tests */
            dumped = true;
            usb_dump();
        }

        for (int i = 0; i < nhcs; i++)
            hcs[i].ops->poll(&hcs[i]);
        for (int i = 0; i < USB_MAX_DEVS; i++)
            if (devs[i].used)
                for (int e = 0; e < USB_MAX_EPS; e++)
                    if (devs[i].ep[e].used)
                        hid_poll(&devs[i].ep[e].hid, now);
        if (now - last_scan > 25) {                            /* root ports: 4 times a second */
            last_scan = now;
            for (int i = 0; i < nhcs; i++)
                scan_root(&hcs[i]);
        }
        if (now - last_hub > 100) {                            /* hub ports: once a second */
            last_hub = now;
            hubs_scan_all();
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

/* ------------------------------------------------------------------ */
/* Information                                                         */
/* ------------------------------------------------------------------ */

static const char *kind_name(const struct usb_dev *d)
{
    for (int e = 0; e < USB_MAX_EPS; e++)
        if (d->ep[e].used) {
            int k = d->ep[e].hid.kind;
            return k == HID_KEYBOARD ? "keyboard" : k == HID_MOUSE ? "mouse" : "tablet";
        }
    return d->is_hub ? "hub" : "no supported interface";
}

int usb_describe(int index, char *buf, size_t n)
{
    int cnt = 0;

    for (int i = 0; i < USB_MAX_DEVS; i++) {
        if (!devs[i].used)
            continue;
        if (cnt++ != index)
            continue;
        const char *k = kind_name(&devs[i]);
        ksnprintf(buf, n, "%c%s  %04x:%04x  %s", k[0] - 32, k + 1, devs[i].vid, devs[i].pid, devs[i].hc->name);
        return 1;
    }
    return 0;
}

void usb_dump(void)
{
    if (!nhcs) {
        kprintf("No USB controller running (xHCI/EHCI/OHCI/UHCI; boot option \"nousb\" disables USB).\n");
        return;
    }
    for (int i = 0; i < nhcs; i++)
        kprintf("%s  ports %d\n", hcs[i].name, hcs[i].nports);
    int cnt = 0;
    for (int i = 0; i < USB_MAX_DEVS; i++) {
        if (!devs[i].used)
            continue;
        cnt++;
        kprintf("  %*s%s port %d  %04x:%04x  %s speed  %s\n", devs[i].depth * 2, "", devs[i].hc->name,
                devs[i].port, devs[i].vid, devs[i].pid, speed_name(devs[i].speed), kind_name(&devs[i]));
    }
    if (!cnt)
        kprintf("  no devices\n");
}
