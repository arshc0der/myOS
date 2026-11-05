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

#define MAXA 4
static struct wifi_adapter *wifis[MAXA];
static int nwifi;
static struct bt_adapter *bts[MAXA];
static int nbt;

int wifi_register(struct wifi_adapter *a) { if (nwifi >= MAXA) return -1; wifis[nwifi++] = a; return 0; }
int wifi_count(void) { return nwifi; }
struct wifi_adapter *wifi_get(int i) { return i >= 0 && i < nwifi ? wifis[i] : NULL; }
int bt_register(struct bt_adapter *a) { if (nbt >= MAXA) return -1; bts[nbt++] = a; return 0; }
int bt_count(void) { return nbt; }
struct bt_adapter *bt_get(int i) { return i >= 0 && i < nbt ? bts[i] : NULL; }

/* PCI class 0x02 (network), subclass 0x80 = "other network controller", which is where Wi-Fi cards live */
int wifi_detect_hardware(char names[][40], int max)
{
    struct pci_device list[64];
    int n = pci_scan(list, 64), found = 0;

    for (int i = 0; i < n && found < max; i++)
        if (list[i].cls == 0x02 && list[i].sub == 0x80)
            ksnprintf(names[found++], 40, "%s %04x:%04x", pci_vendor_name(list[i].vendor), list[i].vendor, list[i].device);
    return found;
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
