/*
 * wireless.c - registries for Wi-Fi and Bluetooth adapters, plus hardware detection.
 *
 * MyOS has NO Wi-Fi or Bluetooth driver. These registries are the place where a driver plugs in
 * (see docs/NETWORK.md); the Settings pages and shell commands already use them. Until a driver
 * registers, the pages show which radios were found and say that no driver exists.
 */
#include "net.h"
#include "hw.h"
#include "usbcore.h"
#include "kstring.h"
#include "kprintf.h"
#include "timer.h"

#define MAXA 4
static struct wifi_adapter *wifis[MAXA];
static int nwifi;
static struct bt_adapter *bts[MAXA];
static int nbt;

static bool wifi_on = true, bt_on = true;
void wifi_set_enabled(bool on) { wifi_on = on; }
bool wifi_enabled(void) { return wifi_on; }
void bt_set_enabled(bool on) { bt_on = on; }
bool bt_enabled(void) { return bt_on; }

int wifi_register(struct wifi_adapter *a) { if (nwifi >= MAXA) return -1; wifis[nwifi++] = a; return 0; }
int wifi_count(void) { return nwifi; }
struct wifi_adapter *wifi_get(int i) { return i >= 0 && i < nwifi ? wifis[i] : NULL; }
int bt_register(struct bt_adapter *a) { if (nbt >= MAXA) return -1; bts[nbt++] = a; return 0; }
int bt_count(void) { return nbt; }
struct bt_adapter *bt_get(int i) { return i >= 0 && i < nbt ? bts[i] : NULL; }

/* Wi-Fi chip makers: PCI vendor id -> family name (for the Settings page; there is still no driver) */
static const struct { uint16_t vid; const char *name; } wifi_vendors[] = {
    { 0x8086, "Intel Wi-Fi" }, { 0x10EC, "Realtek Wi-Fi" }, { 0x168C, "Qualcomm Atheros Wi-Fi" },
    { 0x17CB, "Qualcomm Wi-Fi" }, { 0x14E4, "Broadcom Wi-Fi" }, { 0x14C3, "MediaTek Wi-Fi" },
    { 0x1814, "Ralink/MediaTek Wi-Fi" }, { 0x1969, "Atheros Wi-Fi" }, { 0x11AB, "Marvell Wi-Fi" },
};
/* USB Wi-Fi sticks: vendor ids that mostly make Wi-Fi adapters (class 0xFF "vendor specific"), skipping storage etc. */
static const struct { uint16_t vid; const char *name; } usb_wifi_vendors[] = {
    { 0x0BDA, "Realtek" }, { 0x148F, "Ralink/MediaTek" }, { 0x0E8D, "MediaTek" }, { 0x0CF3, "Qualcomm Atheros" },
    { 0x2357, "TP-Link" }, { 0x0846, "Netgear" }, { 0x2001, "D-Link" }, { 0x7392, "Edimax" },
    { 0x0B05, "ASUS" }, { 0x0411, "Buffalo" }, { 0x050D, "Belkin" }, { 0x0DF6, "Sitecom" },
    { 0x13B1, "Linksys" }, { 0x0A5C, "Broadcom" }, { 0x2C4E, "Mercusys" }, { 0x0489, "Foxconn" },
};

static char hw_names[6][40];
static int hw_n;
static uint32_t hw_t = (uint32_t)-1;

static void hw_scan(void)
{
    struct pci_device list[64];
    int n = pci_scan(list, 64);

    hw_n = 0;
    for (int i = 0; i < n && hw_n < 6; i++) {
        if (!(list[i].cls == 0x02 && list[i].sub == 0x80))      /* "other network controller" = Wi-Fi card */
            continue;
        const char *fam = pci_vendor_name(list[i].vendor);
        for (unsigned k = 0; k < sizeof wifi_vendors / sizeof wifi_vendors[0]; k++)
            if (wifi_vendors[k].vid == list[i].vendor)
                fam = wifi_vendors[k].name;
        ksnprintf(hw_names[hw_n++], 40, "Built-in %s %04x:%04x", fam, list[i].vendor, list[i].device);
    }
    for (int i = 0; hw_n < 6; i++) {
        uint16_t vid, pid;
        uint8_t cls;
        if (!usb_find_unclaimed(i, &vid, &pid, &cls))
            break;
        if (cls != 0 && cls != 0xFF && cls != 0xEF)
            continue;
        for (unsigned k = 0; k < sizeof usb_wifi_vendors / sizeof usb_wifi_vendors[0]; k++)
            if (usb_wifi_vendors[k].vid == vid) {
                ksnprintf(hw_names[hw_n++], 40, "USB %s Wi-Fi %04x:%04x", usb_wifi_vendors[k].name, vid, pid);
                break;
            }
    }
}

/* Cached for two seconds: the Control Center asks every frame. */
int wifi_hw_count(void)
{
    uint32_t now = timer_ticks();
    if (hw_t == (uint32_t)-1 || now - hw_t >= 200) {
        hw_t = now;
        hw_scan();
    }
    return hw_n;
}

const char *wifi_hw_name(int i) { return i >= 0 && i < hw_n ? hw_names[i] : ""; }

int wifi_detect_hardware(char names[][40], int max)
{
    int n = wifi_hw_count();
    for (int i = 0; i < n && i < max; i++)
        strlcpy(names[i], hw_names[i], 40);
    return n < max ? n : max;
}

/* USB wireless controllers (class 0xE0) are Bluetooth dongles */
int bt_detect_hardware(char names[][40], int max)
{
    int found = 0;
    for (int i = 0; i < max; i++) {
        uint16_t vid, pid;
        if (!usb_find_class(0xE0, i, &vid, &pid))
            break;
        ksnprintf(names[found++], 40, "USB Bluetooth %04x:%04x", vid, pid);
    }
    return found;
}
