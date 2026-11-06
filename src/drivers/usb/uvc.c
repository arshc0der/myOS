/*
 * uvc.c - USB video class (UVC) driver: USB webcams and the camera built into most laptops.
 *
 * What it does:
 *   1. reads the video-streaming descriptors: formats (YUY2 and MJPEG) and frame sizes,
 *   2. picks the frame size closest to 320 x 240 (YUY2 preferred),
 *   3. negotiates it with the camera (VS_PROBE / VS_COMMIT),
 *   4. selects the alternate setting with enough bandwidth, starts an isochronous stream (xHCI only),
 *   5. assembles the packets into frames; grab() converts the newest frame to 320 x 240 pixels.
 *
 * Limits: only xHCI controllers (USB 3 ports and most laptop cameras) have isochronous transfers;
 * SuperSpeed cameras are not handled (USB 2 cameras are); no zoom/focus/brightness controls; the
 * camera streams from the moment it is plugged in. Not tested on real cameras yet.
 */
#include "usbcore.h"
#include "camera.h"
#include "image.h"
#include "heap.h"
#include "kstring.h"
#include "kprintf.h"
#include "log.h"

#define MAX_FRAMES 24
#define MAX_ALTS   12
#define OUT_W 320
#define OUT_H 240

struct vframe { uint8_t fmt, idx; bool mjpeg; uint16_t w, h; uint32_t interval, maxbuf; };
struct valt { uint8_t alt, ep, interval; uint16_t raw; int payload; };

struct uvc {
    bool used, alive;
    struct usb_dev *dev;
    struct camera cam;
    char name[40];
    int w, h;
    bool mjpeg;
    /* frame assembly */
    uint8_t *acc, *ready;
    uint32_t cap, acc_len, ready_len, frames;
    int fid;
    bool locked, have;
};

static struct uvc cams[2];

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void frame_done(struct uvc *u)
{
    uint32_t need = (uint32_t)(u->w * u->h * 2);

    if (u->acc_len && !u->locked && (u->mjpeg ? u->acc_len > 100 : u->acc_len >= need)) {
        uint32_t n = u->acc_len < u->cap ? u->acc_len : u->cap;
        memcpy(u->ready, u->acc, n);
        u->ready_len = n;
        u->have = true;
        u->frames++;
    }
    u->acc_len = 0;
}

/* One isochronous packet group: UVC payload header + data */
static void on_packet(void *ctx, const uint8_t *p, int len)
{
    struct uvc *u = ctx;

    if (len < 2)
        return;
    int hl = p[0], flags = p[1];
    if (hl < 2 || hl > len)
        return;
    int fid = flags & 1;
    if (u->fid >= 0 && fid != u->fid)
        frame_done(u);                          /* the frame id toggles at every new frame */
    u->fid = fid;
    if (flags & 0x40) {                         /* error bit: drop this frame */
        u->acc_len = 0;
        return;
    }
    int n = len - hl;
    if (n > 0 && u->acc_len + (uint32_t)n <= u->cap) {
        memcpy(u->acc + u->acc_len, p + hl, (size_t)n);
        u->acc_len += (uint32_t)n;
    }
    if (flags & 2)
        frame_done(u);                          /* end of frame */
}

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static int uvc_grab(struct camera *c, uint32_t *px)
{
    struct uvc *u = c->priv;

    if (!u->alive || !u->have)
        return -1;
    u->locked = true;
    if (u->mjpeg) {
        struct gfx_surface *s = image_from_jpeg(u->ready, u->ready_len);
        if (s) {
            for (int y = 0; y < OUT_H; y++)
                for (int x = 0; x < OUT_W; x++)
                    px[y * OUT_W + x] = s->px[(y * s->h / OUT_H) * s->stride + x * s->w / OUT_W];
            gfx_surface_free(s);
        }
    } else {
        for (int y = 0; y < OUT_H; y++) {
            const uint8_t *row = u->ready + (size_t)(y * u->h / OUT_H) * (size_t)u->w * 2;
            for (int x = 0; x < OUT_W; x++) {
                int sx = x * u->w / OUT_W, pair = sx & ~1;
                int Y = row[sx * 2] - 16, U = row[pair * 2 + 1] - 128, V = row[pair * 2 + 3] - 128;
                int r = (298 * Y + 409 * V + 128) >> 8;
                int g = (298 * Y - 100 * U - 208 * V + 128) >> 8;
                int b = (298 * Y + 516 * U + 128) >> 8;
                px[y * OUT_W + x] = GFX_RGB(clamp8(r), clamp8(g), clamp8(b));
            }
        }
    }
    u->locked = false;
    return 0;
}

bool uvc_attach(struct usb_dev *d, int total)
{
    const uint8_t *c = d->buf;
    struct vframe fr[MAX_FRAMES];
    struct valt alts[MAX_ALTS];
    int nf = 0, na = 0, vsif = -1, curfmt = 0, cfgval = c[5];
    bool curmj = false, cur_ok = false;
    uint16_t bcd = 0x0100;
    uint8_t cls_if = 0, sub_if = 0, cur_alt = 0, cur_ifnum = 0;

    if (!d->hc->ops->iso_start) {
        log_info("usb", "%s port %d: webcam found, but %s has no isochronous transfers (xHCI needed)",
                 d->hc->name, d->port, d->hc->name);
        return false;
    }
    if (d->speed == USB_SUPER) {
        log_info("usb", "%s port %d: SuperSpeed webcam not supported yet", d->hc->name, d->port);
        return false;
    }
    /* ---- read the descriptors (they are in d->buf; control transfers below would overwrite them) ---- */
    for (int i = 0; i + 2 <= total && c[i] >= 2;) {
        int len = c[i], type = c[i + 1];
        if (type == 4 && len >= 9) {
            cls_if = c[i + 5];
            sub_if = c[i + 6];
            cur_ifnum = c[i + 2];
            cur_alt = c[i + 3];
            if (cls_if == 14 && sub_if == 2 && vsif < 0)
                vsif = cur_ifnum;
        } else if (type == 0x24 && cls_if == 14) {
            int st = c[i + 2];
            if (sub_if == 1 && st == 1 && len >= 5)
                bcd = (uint16_t)(c[i + 3] | (c[i + 4] << 8));
            if (sub_if == 2 && cur_ifnum == vsif) {
                if ((st == 4 || st == 6) && len >= 21) {
                    static const uint8_t yuy2[4] = { 0x59, 0x55, 0x59, 0x32 };
                    curfmt = c[i + 3];
                    curmj = st == 6;
                    cur_ok = curmj || memcmp(c + i + 5, yuy2, 4) == 0;
                } else if ((st == 5 || st == 7) && len >= 26 && cur_ok && nf < MAX_FRAMES) {
                    fr[nf].fmt = (uint8_t)curfmt;
                    fr[nf].idx = c[i + 3];
                    fr[nf].mjpeg = curmj;
                    fr[nf].w = (uint16_t)(c[i + 5] | (c[i + 6] << 8));
                    fr[nf].h = (uint16_t)(c[i + 7] | (c[i + 8] << 8));
                    fr[nf].maxbuf = le32(c + i + 17);
                    fr[nf].interval = le32(c + i + 21);
                    nf++;
                }
            }
        } else if (type == 5 && len >= 7 && cls_if == 14 && sub_if == 2 && cur_ifnum == vsif && cur_alt > 0 &&
                   (c[i + 3] & 3) == 1 && (c[i + 2] & 0x80) && na < MAX_ALTS) {
            alts[na].alt = cur_alt;
            alts[na].ep = c[i + 2] & 15;
            alts[na].raw = (uint16_t)(c[i + 4] | (c[i + 5] << 8));
            alts[na].interval = c[i + 6];
            alts[na].payload = (alts[na].raw & 0x7FF) * (1 + ((alts[na].raw >> 11) & 3));
            na++;
        }
        i += len;
    }
    if (vsif < 0 || !nf || !na) {
        log_info("usb", "%s port %d: webcam has no usable YUY2/MJPEG format (found %d frames, %d alt settings)",
                 d->hc->name, d->port, nf, na);
        return false;
    }
    /* ---- choose the frame: closest to 320 x 240, YUY2 preferred, nothing bigger than 640 wide for YUY2 ---- */
    int best = -1, bs = 1 << 30;
    for (int i = 0; i < nf; i++) {
        if (!fr[i].mjpeg && fr[i].w > 640)
            continue;
        int dw = fr[i].w > OUT_W ? fr[i].w - OUT_W : OUT_W - fr[i].w;
        int dh = fr[i].h > OUT_H ? fr[i].h - OUT_H : OUT_H - fr[i].h;
        int s = dw + dh + (fr[i].mjpeg ? 200 : 0);
        if (s < bs) { bs = s; best = i; }
    }
    if (best < 0) {
        log_info("usb", "webcam: no suitable frame size");
        return false;
    }
    struct vframe *f = &fr[best];

    d->claimed = true;
    if (usb_ctrl(d, 0x00, 9, (uint16_t)cfgval, 0, 0) < 0) {                  /* SET_CONFIGURATION */
        log_warn("usb", "webcam: SET_CONFIGURATION failed");
        goto fail;
    }
    /* ---- negotiate (VS_PROBE_CONTROL = 1, VS_COMMIT_CONTROL = 2) ---- */
    int plen = bcd >= 0x0150 ? 48 : bcd >= 0x0110 ? 34 : 26;
    memset(d->buf, 0, 64);
    d->buf[0] = 1;                                                           /* bmHint: dwFrameInterval is fixed */
    d->buf[2] = f->fmt;
    d->buf[3] = f->idx;
    memcpy(d->buf + 4, &f->interval, 4);
    if (usb_ctrl(d, 0x21, 0x01, 0x0100, (uint16_t)vsif, (uint16_t)plen) < 0 ||        /* SET_CUR probe */
        usb_ctrl(d, 0xA1, 0x81, 0x0100, (uint16_t)vsif, (uint16_t)plen) < 0) {        /* GET_CUR probe */
        log_warn("usb", "webcam: probe failed");
        goto fail;
    }
    uint32_t maxpay = le32(d->buf + 22);
    if (usb_ctrl(d, 0x21, 0x01, 0x0200, (uint16_t)vsif, (uint16_t)plen) < 0) {        /* SET_CUR commit */
        log_warn("usb", "webcam: commit failed");
        goto fail;
    }
    /* ---- alternate setting: the smallest one that carries the payload, else the biggest ---- */
    int pick = -1;
    for (int i = 0; i < na; i++) {
        if (alts[i].payload >= (int)maxpay && (pick < 0 || alts[i].payload < alts[pick].payload))
            pick = i;
    }
    if (pick < 0)
        for (int i = 0; i < na; i++)
            if (pick < 0 || alts[i].payload > alts[pick].payload)
                pick = i;
    struct uvc *u = NULL;
    for (int i = 0; i < 2; i++)
        if (!cams[i].used) { u = &cams[i]; break; }
    if (!u) {
        log_warn("usb", "webcam: too many cameras");
        goto fail;
    }
    memset(u, 0, sizeof *u);
    u->cap = f->maxbuf ? f->maxbuf + 4096 : (uint32_t)f->w * f->h * 2 + 4096;
    if (u->cap > 2 * 1024 * 1024)
        u->cap = 2 * 1024 * 1024;
    u->acc = kmalloc(u->cap);
    u->ready = kmalloc(u->cap);
    if (!u->acc || !u->ready) {
        kfree(u->acc);
        kfree(u->ready);
        goto fail;
    }
    u->used = u->alive = true;
    u->dev = d;
    u->w = f->w;
    u->h = f->h;
    u->mjpeg = f->mjpeg;
    u->fid = -1;
    int burst = (alts[pick].raw >> 11) & 3, mps = alts[pick].raw & 0x7FF;
    if (d->hc->ops->iso_start(d, alts[pick].ep, mps, burst, alts[pick].interval, on_packet, u) < 0) {
        log_warn("usb", "webcam: could not start the isochronous stream");
        kfree(u->acc);
        kfree(u->ready);
        u->used = false;
        goto fail;
    }
    if (usb_ctrl(d, 0x01, 0x0B, alts[pick].alt, (uint16_t)vsif, 0) < 0) {          /* SET_INTERFACE: start streaming */
        log_warn("usb", "webcam: SET_INTERFACE failed");
        if (d->hc->ops->iso_stop)
            d->hc->ops->iso_stop(d);
        kfree(u->acc);
        kfree(u->ready);
        u->used = false;
        goto fail;
    }
    ksnprintf(u->name, sizeof u->name, "USB camera %04x:%04x (%dx%d %s)", d->vid, d->pid, f->w, f->h,
              f->mjpeg ? "MJPEG" : "YUY2");
    u->cam.name = u->name;
    u->cam.w = OUT_W;
    u->cam.h = OUT_H;
    u->cam.grab = uvc_grab;
    u->cam.priv = u;
    camera_register(&u->cam);
    log_info("usb", "%s port %d: webcam %04x:%04x %dx%d %s, %d bytes per interval (%s speed)", d->hc->name, d->port,
             d->vid, d->pid, f->w, f->h, f->mjpeg ? "MJPEG" : "YUY2", alts[pick].payload, usb_speed_name(d->speed));
    return true;
fail:
    d->claimed = false;
    return false;
}

void uvc_detach(struct usb_dev *d)
{
    for (int i = 0; i < 2; i++) {
        struct uvc *u = &cams[i];
        if (u->used && u->dev == d) {
            u->alive = false;
            camera_unregister(&u->cam);
            kfree(u->acc);
            kfree(u->ready);
            u->used = false;
        }
    }
}
