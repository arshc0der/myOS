/*
 * usb.h - USB support (xHCI host controller + HID keyboards, mice, tablets
 * and touchpads that look like a mouse).
 *
 * The "usb" kernel task starts at boot, finds the xHCI controller on the
 * PCI bus, enumerates the devices plugged into its ports and turns their
 * reports into the same input events the PS/2 drivers produce (see input.h).
 * Disable it with the boot option "nousb". The shell command `lsusb` lists
 * what was found.
 */
#ifndef USB_H
#define USB_H
#include "types.h"

void usb_start(void);                   /* create the usb task */
void usb_dump(void);                    /* print controller + devices (lsusb) */

/* ---- HID report handling (src/drivers/usb/hid.c) ---- */
enum hid_kind { HID_NONE = 0, HID_KEYBOARD, HID_MOUSE, HID_POINTER_ABS };

struct hid_dev {
    int kind;
    /* keyboard state */
    uint8_t prev[8];
    uint8_t held_mods;
    int held_key;                       /* translated key being repeated */
    uint8_t held_usage;
    uint32_t held_since, last_repeat;
    bool caps;
    /* pointer description (bit offsets inside the report) */
    bool has_id;
    uint8_t rid;
    int8_t  bo[3];                      /* bit offset of buttons 1..3, -1 = none */
    uint16_t bpos[3];
    uint16_t xo, yo, wo;
    uint8_t  xs, ys, ws;                /* field sizes in bits, 0 = absent */
    int32_t  xmin, xmax, ymin, ymax;
    bool rel;
    bool sx, sy;                        /* signed fields */
    /* last values (for absolute devices: only report changes) */
    int32_t lx, ly;
    uint8_t lbuttons;
};

/* desc == NULL: boot protocol (kind tells keyboard or mouse).
 * Otherwise the HID report descriptor is parsed and kind is detected. */
void hid_setup(struct hid_dev *h, int kind, const uint8_t *desc, int len);
void hid_report(struct hid_dev *h, const uint8_t *r, int len);
void hid_poll(struct hid_dev *h, uint32_t now_ticks);   /* key repeat */

#endif
