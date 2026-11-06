/*
 * btusb.c - Bluetooth over USB (HCI transport, class 0xE0 / 1 / 1).
 *
 * What works: the dongle is reset and registered as a Bluetooth adapter; scan() runs a classic
 * inquiry and returns nearby devices (address, name when the device sends one, kind from the class code).
 * What does not: pairing, audio and data links. Those need ACL data on bulk endpoints and the L2CAP /
 * SDP layers; the USB stack of MyOS only has control and interrupt-IN transfers so far.
 * Commands go over the control endpoint (class request), events come in on the interrupt-IN endpoint.
 */
#include "usbcore.h"
#include "net.h"
#include "pmm.h"
#include "timer.h"
#include "kstring.h"
#include "kprintf.h"
#include "log.h"

#define MAXDEV 8

static struct {
    struct usb_dev *d;
    bool ready;
    uint8_t ev[262];
    int have, need;                         /* reassembly of one event */
    struct bt_device list[MAXDEV];
    int found;
    volatile bool done;
    char name[40];
} bt;

static struct bt_adapter adapter;

static int hci_cmd(uint16_t opcode, const uint8_t *par, int plen)
{
    struct usb_dev *d = bt.d;
    if (!d || plen > 200)
        return -1;
    d->buf[0] = (uint8_t)opcode;
    d->buf[1] = (uint8_t)(opcode >> 8);
    d->buf[2] = (uint8_t)plen;
    for (int i = 0; i < plen; i++)
        d->buf[3 + i] = par[i];
    return d->hc->ops->ctrl(d, 0x20, 0, 0, 0, (uint16_t)(3 + plen));
}

static const char *kind_of(uint32_t cod)
{
    switch ((cod >> 8) & 0x1F) {
    case 1: return "Computer";
    case 2: return "Phone";
    case 3: return "Network";
    case 4: return "Audio";
    case 5: return "Input device";
    case 6: return "Imaging";
    case 7: return "Wearable";
    default: return "Bluetooth device";
    }
}

static void add(const uint8_t *addr, uint32_t cod, const uint8_t *eir, int eirlen)
{
    int i;
    for (i = 0; i < bt.found; i++)
        if (!memcmp(bt.list[i].addr, addr, 6))
            break;
    if (i == bt.found) {
        if (bt.found >= MAXDEV)
            return;
        bt.found++;
        memset(&bt.list[i], 0, sizeof bt.list[i]);
        memcpy(bt.list[i].addr, addr, 6);
        ksnprintf(bt.list[i].name, sizeof bt.list[i].name, "%s %02x:%02x:%02x", kind_of(cod), addr[2], addr[1], addr[0]);
    }
    for (int k = 0; eir && k + 1 < eirlen;) {          /* extended inquiry data: look for the name */
        int l = eir[k];
        if (!l)
            break;
        if ((eir[k + 1] == 0x09 || eir[k + 1] == 0x08) && k + l < eirlen + 1) {
            int n = l - 1 < 31 ? l - 1 : 31;
            memcpy(bt.list[i].name, eir + k + 2, (size_t)n);
            bt.list[i].name[n] = 0;
        }
        k += l + 1;
    }
}

static void handle(const uint8_t *e, int len)
{
    if (len < 2)
        return;
    switch (e[0]) {
    case 0x01:                                          /* inquiry complete */
        bt.done = true;
        break;
    case 0x02: case 0x22: {                             /* inquiry result (with RSSI) */
        int n = e[2], step = 14;
        for (int i = 0; i < n && 3 + (i + 1) * step <= len; i++) {
            const uint8_t *r = e + 3 + i * step;
            uint32_t cod = (uint32_t)(r[9] | (r[10] << 8) | (r[11] << 16));
            if (e[0] == 0x02) cod = (uint32_t)(r[9] | (r[10] << 8) | (r[11] << 16));
            add(r, cod, NULL, 0);
        }
        break;
    }
    case 0x2F:                                          /* extended inquiry result */
        if (len >= 17) {
            const uint8_t *r = e + 3;
            uint32_t cod = (uint32_t)(r[9] | (r[10] << 8) | (r[11] << 16));
            add(r, cod, r + 15, len - 3 - 15);
        }
        break;
    }
}

/* called by usb_report for the interrupt-IN endpoint: events may arrive in several packets */
void btusb_event(const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        if (bt.have == 0 && i == 0 && false)
            break;
        bt.ev[bt.have++] = data[i];
        if (bt.have == 2)
            bt.need = 2 + bt.ev[1];
        if (bt.have >= 2 && bt.have >= bt.need) {
            handle(bt.ev, bt.have);
            bt.have = 0;
        }
        if (bt.have >= (int)sizeof bt.ev)
            bt.have = 0;
    }
}

static int bt_scan(struct bt_adapter *a, struct bt_device *out, int max)
{
    static const uint8_t mode[1] = { 2 };                             /* extended inquiry results */
    static const uint8_t inq[5] = { 0x33, 0x8B, 0x9E, 2, 0 };         /* general access code, 2.5 s, no limit */
    (void)a;
    if (!bt.ready || !bt_enabled())
        return 0;
    bt.found = 0;
    bt.done = false;
    bt.have = 0;
    hci_cmd(0x0C45, mode, 1);
    timer_sleep_ms(30);
    if (hci_cmd(0x0401, inq, 5) < 0)
        return 0;
    for (int i = 0; i < 400 && !bt.done; i++)
        timer_sleep_ms(10);
    int n = bt.found < max ? bt.found : max;
    for (int i = 0; i < n; i++)
        out[i] = bt.list[i];
    return n;
}

/* Called from the USB core for a device that has a Bluetooth interface. `total` = configuration length. */
bool btusb_attach(struct usb_dev *d, int total)
{
    const uint8_t *c = d->buf;
    int epn = -1, mps = 16, itv = 1, iface = -1;
    uint8_t cfg = c[5];

    if (bt.ready)
        return false;
    for (int i = 0; i + 2 <= total && c[i] >= 2; i += c[i]) {
        if (c[i + 1] == 4 && c[i] >= 9)
            iface = (c[i + 5] == 0xE0 && c[i + 6] == 1 && c[i + 7] == 1 && c[i + 3] == 0) ? c[i + 2] : -1;
        else if (c[i + 1] == 5 && c[i] >= 7 && iface >= 0 && (c[i + 3] & 3) == 3 && (c[i + 2] & 0x80) && epn < 0) {
            epn = c[i + 2] & 15;
            mps = (c[i + 4] | (c[i + 5] << 8)) & 0x7FF;
            itv = c[i + 6];
        }
    }
    if (epn < 0)
        return false;
    if (d->hc->ops->ctrl(d, 0x00, 9, cfg, 0, 0) < 0)
        return false;
    struct usb_ep *e = &d->ep[0];
    memset(e, 0, sizeof *e);
    e->num = epn;
    e->mps = mps > 64 ? 64 : mps;
    e->interval = itv;
    e->iface = (uint8_t)iface;
    e->bt = true;
    e->buf = usb_dma_pages(1, NULL);
    if (!e->buf)
        return false;
    e->used = true;
    if (d->hc->ops->configure && d->hc->ops->configure(d) < 0) {
        e->used = false;
        return false;
    }
    d->hc->ops->intr_start(d, e);
    bt.d = d;
    bt.ready = true;
    hci_cmd(0x0C03, NULL, 0);                           /* HCI_Reset */
    ksnprintf(bt.name, sizeof bt.name, "USB Bluetooth %04x:%04x", d->vid, d->pid);
    adapter.name = bt.name;
    adapter.scan = bt_scan;
    bt_register(&adapter);
    log_info("usb", "%s port %d: Bluetooth adapter %04x:%04x (discovery only)", d->hc->name, d->port, d->vid, d->pid);
    return true;
}
