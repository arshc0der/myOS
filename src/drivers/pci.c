/*
 * pci.c - lists the devices on the PCI bus (network card, disk controller,
 * graphics adapter ...).
 *
 * To read a PCI register, write its address to port 0xCF8 and read the
 * answer from port 0xCFC. If no device is present the answer is 0xFFFF.
 */
#include "hw.h"
#include "io.h"

static uint32_t pci_read(uint32_t bus, uint32_t slot, uint32_t func, uint32_t off)
{
    outl(0xCF8, (1u << 31) | (bus << 16) | (slot << 11) | (func << 8) | (off & 0xFC));
    return inl(0xCFC);
}

uint32_t pci_cfg_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    return pci_read(bus, slot, func, off);
}

void pci_cfg_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v)
{
    outl(0xCF8, (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

const char *pci_vendor_name(uint16_t id)
{
    switch (id) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD/ATI";
    case 0x10DE: return "NVIDIA";
    case 0x10EC: return "Realtek";
    case 0x14E4: return "Broadcom";
    case 0x168C: return "Qualcomm Atheros";
    case 0x1106: return "VIA";
    case 0x1234: return "QEMU/Bochs";
    case 0x1B36: return "Red Hat (QEMU)";
    case 0x1AF4: return "Red Hat (virtio)";
    case 0x80EE: return "VirtualBox";
    case 0x15AD: return "VMware";
    default:     return "Unknown vendor";
    }
}

const char *pci_class_name(uint8_t cls, uint8_t sub)
{
    switch (cls) {
    case 0x00: return "Unclassified device";
    case 0x01:
        switch (sub) {
        case 0x00: return "SCSI controller";
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVM controller";
        default:   return "Storage controller";
        }
    case 0x02: return sub == 0 ? "Ethernet controller" : "Network controller";
    case 0x03: return sub == 0 ? "VGA controller" : "Display controller";
    case 0x04: return "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06:
        switch (sub) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        default:   return "Other bridge";
        }
    case 0x07: return "Communication controller";
    case 0x08: return "System peripheral";
    case 0x09: return "Input device";
    case 0x0C:
        switch (sub) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default:   return "Serial bus controller";
        }
    default:   return "Other device";
    }
}

int pci_scan(struct pci_device *out, int max)
{
    int count = 0;

    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            if ((pci_read(bus, slot, 0, 0) & 0xFFFF) == 0xFFFF)
                continue;
            uint32_t header = (pci_read(bus, slot, 0, 0x0C) >> 16) & 0xFF;
            uint32_t funcs = (header & 0x80) ? 8 : 1;      /* multi-function? */

            for (uint32_t fn = 0; fn < funcs; fn++) {
                uint32_t id = pci_read(bus, slot, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF)
                    continue;
                if (out && count < max) {
                    uint32_t cr = pci_read(bus, slot, fn, 8);
                    out[count].bus = (uint8_t)bus;
                    out[count].slot = (uint8_t)slot;
                    out[count].func = (uint8_t)fn;
                    out[count].vendor = (uint16_t)(id & 0xFFFF);
                    out[count].device = (uint16_t)(id >> 16);
                    out[count].cls = (uint8_t)(cr >> 24);
                    out[count].sub = (uint8_t)(cr >> 16);
                }
                count++;
            }
        }
    }
    return count;
}
