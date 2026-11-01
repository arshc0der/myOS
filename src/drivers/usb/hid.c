/*
 * hid.c - turns USB HID reports into input events.
 *
 *  - Keyboards: the 8-byte boot report (modifiers, reserved, 6 key codes).
 *  - Mice: the boot report (buttons, dx, dy, [wheel]).
 *  - Anything else that points (tablets such as the one virtual machines
 *    provide, touchpads that look like a mouse): the report descriptor is
 *    parsed to find where the buttons and the X/Y fields are.
 */
#include "usb.h"
#include "input.h"
#include "timer.h"
#include "kstring.h"

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */

static const char punct[]  = "-=[]\\#;'`,./";
static const char punct_s[] = "_+{}|~:\"~<>?";
static const char digit_s[] = "!@#$%^&*()";

/* Returns the key code the shell/GUI expects, or 0 for "no key". */
static int translate(const struct hid_dev *h, uint8_t u, uint8_t mods)
{
    bool shift = mods & 0x22, ctrl = mods & 0x11;

    if (u >= 0x04 && u <= 0x1D) {
        char c = (char)('a' + (u - 4));
        if (shift != h->caps)
            c = (char)(c - 32);
        if (ctrl)
            c = (char)((c & 31));
        return (unsigned char)c;
    }
    if (u >= 0x1E && u <= 0x27) {
        if (shift)
            return digit_s[u - 0x1E];
        return u == 0x27 ? '0' : '1' + (u - 0x1E);
    }
    if (u >= 0x2D && u <= 0x38)
        return shift ? punct_s[u - 0x2D] : punct[u - 0x2D];
    if (u >= 0x3A && u <= 0x45)
        return KEY_F(u - 0x3A + 1);
    if (u >= 0x59 && u <= 0x61)
        return '1' + (u - 0x59);
    switch (u) {
    case 0x28: case 0x58: return '\n';
    case 0x29: return 27;
    case 0x2A: return '\b';
    case 0x2B: return '\t';
    case 0x2C: return ' ';
    case 0x49: return KEY_INSERT;
    case 0x4A: return KEY_HOME;
    case 0x4B: return KEY_PGUP;
    case 0x4C: return KEY_DELETE;
    case 0x4D: return KEY_END;
    case 0x4E: return KEY_PGDN;
    case 0x4F: return KEY_RIGHT;
    case 0x50: return KEY_LEFT;
    case 0x51: return KEY_DOWN;
    case 0x52: return KEY_UP;
    case 0x54: return '/';
    case 0x55: return '*';
    case 0x56: return '-';
    case 0x57: return '+';
    case 0x62: return '0';
    case 0x63: return '.';
    }
    return 0;
}

static void emit_key(int key, uint8_t mods)
{
    struct input_event ev = { .type = INPUT_KEY, .key = key };

    ev.mods = (uint8_t)(((mods & 0x22) ? MOD_SHIFT : 0) | ((mods & 0x11) ? MOD_CTRL : 0) |
                        ((mods & 0x44) ? MOD_ALT : 0));
    input_push(&ev);
}

static void keyboard_report(struct hid_dev *h, const uint8_t *r, int len)
{
    uint8_t cur[8];

    if (len < 3)
        return;
    memset(cur, 0, sizeof cur);
    memcpy(cur, r, (size_t)(len > 8 ? 8 : len));
    if (cur[2] == 1)                            /* phantom/rollover error */
        return;
    uint8_t mods = cur[0];

    for (int i = 2; i < 8; i++) {
        uint8_t u = cur[i];
        if (u < 4 || memchr(h->prev + 2, u, 6))
            continue;                           /* empty or already held */
        if (u == 0x39) {                        /* caps lock toggles */
            h->caps = !h->caps;
            continue;
        }
        int k = translate(h, u, mods);
        if (!k)
            continue;
        emit_key(k, mods);
        h->held_key = k;
        h->held_usage = u;
        h->held_mods = mods;
        h->held_since = h->last_repeat = timer_ticks();
    }
    if (h->held_key && !memchr(cur + 2, h->held_usage, 6))
        h->held_key = 0;                        /* the repeating key was released */
    memcpy(h->prev, cur, sizeof cur);
}

void hid_poll(struct hid_dev *h, uint32_t now)
{
    if (h->kind != HID_KEYBOARD || !h->held_key)
        return;
    if (now - h->held_since >= 40 && now - h->last_repeat >= 3) {   /* 400 ms, then 30/s */
        h->last_repeat = now;
        emit_key(h->held_key, h->held_mods);
    }
}

/* ------------------------------------------------------------------ */
/* Pointing devices                                                    */
/* ------------------------------------------------------------------ */

static uint32_t get_bits(const uint8_t *r, int len, uint32_t pos, uint32_t size)
{
    uint32_t v = 0;

    for (uint32_t i = 0; i < size; i++) {
        uint32_t bit = pos + i;
        if ((int)(bit >> 3) >= len)
            break;
        if (r[bit >> 3] & (1u << (bit & 7)))
            v |= 1u << i;
    }
    return v;
}

static int32_t sext(uint32_t v, uint32_t size)
{
    if (size < 32 && (v & (1u << (size - 1))))
        v |= ~0u << size;
    return (int32_t)v;
}

static int32_t item_value(const uint8_t *p, int size, bool is_signed)
{
    uint32_t v = 0;

    for (int i = 0; i < size; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return is_signed ? sext(v, (uint32_t)size * 8) : (int32_t)v;
}

/* Walk the report descriptor and remember where buttons and X/Y/wheel are. */
static void parse_desc(struct hid_dev *h, const uint8_t *d, int len)
{
    static uint16_t bitpos[256];
    uint32_t page = 0, rsize = 0, rcount = 0, rid = 0;
    int32_t lmin = 0, lmax = 0;
    uint32_t usages[16];
    int nus = 0;
    int32_t umin = 0, umax = -1;
    bool captured = false, first_app = true;

    memset(bitpos, 0, sizeof bitpos);
    for (int i = 0; i < len;) {
        uint8_t b = d[i];
        int size = b & 3, type = (b >> 2) & 3, tag = b >> 4;
        if (b == 0xFE) {                        /* long item: skip */
            i += 3 + (i + 1 < len ? d[i + 1] : 0);
            continue;
        }
        if (size == 3)
            size = 4;
        if (i + 1 + size > len)
            break;
        const uint8_t *p = d + i + 1;
        i += 1 + size;

        if (type == 1) {                        /* global */
            switch (tag) {
            case 0: page = (uint32_t)item_value(p, size, false); break;
            case 1: lmin = item_value(p, size, true); break;
            case 2: lmax = item_value(p, size, lmin < 0); break;
            case 7: rsize = (uint32_t)item_value(p, size, false); break;
            case 8: rid = (uint32_t)item_value(p, size, false) & 0xFF; break;
            case 9: rcount = (uint32_t)item_value(p, size, false); break;
            }
        } else if (type == 2) {                 /* local */
            uint32_t v = (uint32_t)item_value(p, size, false);
            if (tag == 0 && nus < 16) usages[nus++] = size == 4 ? (v & 0xFFFF) : v;
            else if (tag == 1) umin = (int32_t)v;
            else if (tag == 2) umax = (int32_t)v;
        } else if (type == 0) {                 /* main */
            if (tag == 10 && first_app && nus > 0 && page == 1) {      /* first collection */
                first_app = false;
                if (usages[0] == 6)
                    h->kind = HID_KEYBOARD;
            } else if (tag == 10 && first_app && nus > 0 && page == 0x0D) {
                first_app = false;
                if (usages[0] == 5)             /* Digitizers / Touch Pad */
                    h->tp = true;
            } else if (tag == 8) {              /* input */
                uint32_t flags = (uint32_t)item_value(p, size, false);
                uint32_t total = rsize * rcount;
                bool constant = flags & 1;
                if (!constant && (!captured || rid == h->rid) && h->kind != HID_KEYBOARD) {
                    for (uint32_t k = 0; k < rcount; k++) {
                        uint32_t u;
                        if (nus)
                            u = k < (uint32_t)nus ? usages[k] : usages[nus - 1];
                        else if (umax >= umin)
                            u = (uint32_t)umin + k;
                        else
                            continue;
                        uint32_t pos = bitpos[rid] + k * rsize;
                        bool ours = false;
                        if (page == 9 && u >= 1 && u <= 3 && rsize == 1) {
                            h->bo[u - 1] = 1;
                            h->bpos[u - 1] = (uint16_t)pos;
                            ours = true;
                        } else if (page == 1 && u == 0x30 && !h->xs) {
                            h->xo = (uint16_t)pos; h->xs = (uint8_t)rsize;
                            h->xmin = lmin; h->xmax = lmax; h->sx = lmin < 0;
                            h->rel = flags & 4;
                            ours = true;
                        } else if (page == 1 && u == 0x31 && !h->ys) {
                            h->yo = (uint16_t)pos; h->ys = (uint8_t)rsize;
                            h->ymin = lmin; h->ymax = lmax; h->sy = lmin < 0;
                            ours = true;
                        } else if (h->tp && page == 0x0D && u == 0x42 && !h->ts && rsize == 1) {
                            h->to = (uint16_t)pos; h->ts = 1;               /* tip switch of the first finger */
                            ours = true;
                        } else if (h->tp && page == 0x0D && u == 0x51 && !h->cs && rsize <= 16) {
                            h->ci = (uint16_t)pos; h->cs = (uint8_t)rsize;  /* contact id of the first finger */
                            ours = true;
                        } else if (page == 1 && u == 0x38 && !h->ws) {
                            h->wo = (uint16_t)pos; h->ws = (uint8_t)rsize;
                            ours = true;
                        }
                        if (ours && !captured) {
                            captured = true;
                            h->rid = (uint8_t)rid;
                            h->has_id = rid != 0;
                        }
                    }
                }
                bitpos[rid] = (uint16_t)(bitpos[rid] + total);
            }
            nus = 0;
            umin = 0;
            umax = -1;
        }
    }
    if (h->kind != HID_KEYBOARD)
        h->kind = (h->xs && h->ys) ? ((h->rel || h->tp) ? HID_MOUSE : HID_POINTER_ABS) : HID_NONE;
    if (h->tp && (!h->ts || h->kind != HID_MOUSE))
        h->tp = false;                          /* no tip switch: cannot be used as a touchpad */
}

void hid_setup(struct hid_dev *h, int kind, const uint8_t *desc, int len)
{
    memset(h, 0, sizeof *h);
    h->bo[0] = h->bo[1] = h->bo[2] = -1;
    h->kind = kind;
    h->lx = h->ly = -1;
    if (desc) {
        h->kind = HID_NONE;
        parse_desc(h, desc, len);
    } else if (kind == HID_MOUSE) {             /* boot mouse layout */
        for (int i = 0; i < 3; i++) {
            h->bo[i] = 1;
            h->bpos[i] = (uint16_t)i;
        }
        h->xo = 8;  h->xs = 8;  h->sx = true;
        h->yo = 16; h->ys = 8;  h->sy = true;
        h->wo = 24; h->ws = 8;
        h->rel = true;
    }
}

/*
 * Touchpad: the pad reports where the first finger is. We turn finger motion
 * into relative pointer motion and a quick touch without motion into a click.
 */
static void touchpad_report(struct hid_dev *h, const uint8_t *r, int len, uint32_t base, uint8_t buttons)
{
    bool tip = get_bits(r, len, base + h->to, 1) != 0;
    uint32_t xr = get_bits(r, len, base + h->xo, h->xs);
    uint32_t yr = get_bits(r, len, base + h->yo, h->ys);
    int32_t x = h->sx ? sext(xr, h->xs) : (int32_t)xr;
    int32_t y = h->sy ? sext(yr, h->ys) : (int32_t)yr;
    int32_t cid = h->cs ? (int32_t)get_bits(r, len, base + h->ci, h->cs) : 0;
    int32_t range = h->xmax - h->xmin;
    struct input_event ev = { .type = INPUT_MOUSE, .buttons = buttons };
    bool send = false;

    if (range <= 0)
        range = 1;
    if (tip && h->tdown && cid != h->tcid)      /* another finger took over slot 1: start again */
        h->tdown = false;
    if (tip) {
        if (!h->tdown) {
            h->tdown = true;
            h->tcid = cid;
            h->px = x;
            h->py = y;
            h->moved = 0;
            h->t0 = timer_ticks();
        } else {
            int32_t dx = (int32_t)(((int64_t)(x - h->px) * 1600) / range);   /* a full swipe ~ 1600 pixels */
            int32_t dy = (int32_t)(((int64_t)(y - h->py) * 1600) / range);
            h->px = x;
            h->py = y;
            h->moved += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            ev.dx = dx;
            ev.dy = dy;
            send = dx || dy;
        }
    } else if (h->tdown) {                      /* finger lifted */
        h->tdown = false;
        if (h->moved < 12 && timer_ticks() - h->t0 < 25 && !buttons) {   /* tap = click */
            struct input_event down = { .type = INPUT_MOUSE, .buttons = 1 };
            input_push(&down);
            struct input_event up = { .type = INPUT_MOUSE, .buttons = 0 };
            input_push(&up);
            return;
        }
    }
    if (buttons != h->lbuttons)
        send = true;
    if (!send)
        return;
    h->lbuttons = buttons;
    input_push(&ev);
}

static void pointer_report(struct hid_dev *h, const uint8_t *r, int len)
{
    uint32_t base = 0;

    if (h->has_id) {
        if (len < 1 || r[0] != h->rid)
            return;
        base = 8;
    }
    uint8_t buttons = 0;
    for (int i = 0; i < 3; i++)
        if (h->bo[i] >= 0 && get_bits(r, len, base + h->bpos[i], 1))
            buttons |= (uint8_t)(1 << i);
    if (h->tp) {
        touchpad_report(h, r, len, base, buttons);
        return;
    }

    uint32_t xr = get_bits(r, len, base + h->xo, h->xs);
    uint32_t yr = get_bits(r, len, base + h->yo, h->ys);
    int32_t x = h->sx ? sext(xr, h->xs) : (int32_t)xr;
    int32_t y = h->sy ? sext(yr, h->ys) : (int32_t)yr;
    struct input_event ev = { .type = INPUT_MOUSE, .buttons = buttons };

    if (h->kind == HID_MOUSE) {
        ev.dx = x;
        ev.dy = y;
        if (!x && !y && buttons == h->lbuttons)
            return;
    } else {
        int32_t rx = h->xmax - h->xmin, ry = h->ymax - h->ymin;
        if (rx <= 0 || ry <= 0)
            return;
        if (x < h->xmin) x = h->xmin;
        if (y < h->ymin) y = h->ymin;
        if (x > h->xmax) x = h->xmax;
        if (y > h->ymax) y = h->ymax;
        ev.dx = (int32_t)(((uint64_t)(uint32_t)(x - h->xmin) * 32767u) / (uint32_t)rx);
        ev.dy = (int32_t)(((uint64_t)(uint32_t)(y - h->ymin) * 32767u) / (uint32_t)ry);
        ev.pad = 1;                             /* absolute position, 0..32767 */
        if (ev.dx == h->lx && ev.dy == h->ly && buttons == h->lbuttons)
            return;
        h->lx = ev.dx;
        h->ly = ev.dy;
    }
    h->lbuttons = buttons;
    input_push(&ev);
}

void hid_report(struct hid_dev *h, const uint8_t *r, int len)
{
    if (h->kind == HID_KEYBOARD)
        keyboard_report(h, r, len);
    else if (h->kind == HID_MOUSE || h->kind == HID_POINTER_ABS)
        pointer_report(h, r, len);
}
