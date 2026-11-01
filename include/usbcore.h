/*
 * usbcore.h - the USB layer between the host controller drivers (xhci.c,
 * ehci.c, ohci.c, uhci.c) and the class drivers (HID, hubs).
 *
 *   host controller driver  --usb_hc_ops-->  usbcore.c  -->  hid.c / hub code
 *
 * A host controller driver only has to know how to: reset a root port,
 * give a device an address, run a control transfer, and keep an interrupt-IN
 * transfer going on an endpoint. Everything else (reading descriptors,
 * hubs, choosing HID interfaces, hot plug) is shared in usbcore.c.
 */
#ifndef USBCORE_H
#define USBCORE_H
#include "usb.h"
#include "hw.h"

#define USB_LOW    1
#define USB_FULL   2
#define USB_HIGH   3
#define USB_SUPER  4

#define USB_MAX_HC    12
#define USB_MAX_DEVS  24
#define USB_MAX_EPS   3
#define USB_MAX_HUB_PORTS 8

struct usb_hc;
struct usb_dev;

struct usb_ep {
    bool used;
    int num;                    /* endpoint number (0..15), direction is always IN */
    int mps, interval;          /* max packet size, bInterval from the descriptor  */
    uint8_t iface;
    uint8_t *buf;               /* receives the report (one DMA page)               */
    void *hcpriv;               /* the host controller driver's data                */
    struct hid_dev hid;
};

struct usb_dev {
    bool used;
    struct usb_hc *hc;
    struct usb_dev *parent;     /* NULL = plugged into the computer's own port */
    int port;                   /* port number on the parent (or root) hub     */
    int root_port;
    int depth;                  /* 0 = directly on the root hub                 */
    uint32_t route;             /* xHCI route string                            */
    int speed;                  /* USB_LOW .. USB_SUPER                         */
    int addr;                   /* USB address (EHCI/OHCI/UHCI)                 */
    int mps0;                   /* packet size of endpoint 0                    */
    uint16_t vid, pid;
    uint8_t cls;
    bool is_hub, multi_tt;
    int nports;                 /* hubs                                         */
    struct usb_dev *child[USB_MAX_HUB_PORTS + 1];
    uint8_t child_state[USB_MAX_HUB_PORTS + 1];     /* 0 empty, 1 enumerated, 2 failed */
    uint8_t *buf;               /* DMA scratch page for control transfers        */
    void *hcpriv;
    struct usb_ep ep[USB_MAX_EPS];
};

struct usb_hc_ops {
    const char *name;
    bool addr_first;            /* xHCI: the device is addressed before descriptors are read */
    bool no_split;              /* cannot talk to low/full speed devices behind a high speed hub */

    int  (*dev_alloc)(struct usb_dev *d);
    int  (*dev_address)(struct usb_dev *d);                 /* give it an address */
    void (*dev_free)(struct usb_dev *d);
    int  (*set_mps0)(struct usb_dev *d, int mps);           /* optional */
    /* Control transfer. Data (if any) is read from / written to d->buf.
     * Direction comes from bit 7 of rt. Returns 0 or -1. */
    int  (*ctrl)(struct usb_dev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len);
    int  (*configure)(struct usb_dev *d);                   /* all endpoints are known now */
    int  (*intr_start)(struct usb_dev *d, struct usb_ep *e);/* keep an interrupt-IN transfer running */
    void (*intr_stop)(struct usb_dev *d, struct usb_ep *e);
    void (*poll)(struct usb_hc *hc);                        /* process completions */
    /* root hub */
    int  (*rh_connected)(struct usb_hc *hc, int port);      /* 1 if a device we own is present */
    int  (*rh_reset)(struct usb_hc *hc, int port);          /* reset; returns USB_* speed or -1 */
};

struct usb_hc {
    const struct usb_hc_ops *ops;
    char name[24];
    int nports;
    void *priv;
    uint8_t rh_state[32];       /* per root port: 0 empty, 1 enumerated, 2 failed */
    struct usb_dev *rh_dev[32];
};

/* ---- used by host controller drivers ---- */
int  usb_hc_add(const struct usb_hc_ops *ops, const char *name, int nports, void *priv);
void usb_report(struct usb_dev *d, struct usb_ep *e, const uint8_t *data, int len);

/* DMA memory below 4 GiB (identity mapped): zeroed pages. */
void *usb_dma_pages(int pages, paddr_t *phys);
void  usb_dma_free(void *p, int pages);

/* controller probes: return true if a controller was set up */
bool xhci_probe(const struct pci_device *pd);
bool ehci_probe(const struct pci_device *pd);
bool ohci_probe(const struct pci_device *pd);
bool uhci_probe(const struct pci_device *pd);

/* PCI helpers shared by the controller drivers */
void usb_pci_enable(const struct pci_device *pd, bool io);
/* map BAR0 of a memory-mapped controller (EHCI, OHCI): returns NULL on failure */
volatile uint8_t *usb_map_bar0(const struct pci_device *pd);

#endif
