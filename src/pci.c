/*
 * pci.c - lists the devices on the PCI bus (network card, disk
 * controller, graphics adapter, ...).
 *
 * To read a PCI register, write its address to port 0xCF8 and read the
 * answer from port 0xCFC. If no device is present the answer is 0xFFFF.
 */

#include <stdint.h>

extern void console_write(const char *);
extern void console_putc(char);
extern void console_set_color(uint8_t);
extern void console_put_dec(uint32_t);
extern void console_put_hex_plain(uint32_t, int);

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;

    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t pci_read(uint32_t bus, uint32_t slot,
                         uint32_t func, uint32_t offset)
{
    uint32_t address = (1u << 31)
                     | (bus  << 16)
                     | (slot << 11)
                     | (func << 8)
                     | (offset & 0xFC);

    outl(0xCF8, address);
    return inl(0xCFC);
}

static const char *vendor_name(uint32_t id)
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
    case 0x1234: return "QEMU";
    case 0x1B36: return "Red Hat (QEMU)";
    case 0x1AF4: return "Red Hat (virtio)";
    case 0x80EE: return "VirtualBox";
    case 0x15AD: return "VMware";
    default:     return "Unknown vendor";
    }
}

static const char *class_name(uint32_t cls, uint32_t sub)
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
    case 0x02: return (sub == 0x00) ? "Ethernet controller"
                                    : "Network controller";
    case 0x03: return (sub == 0x00) ? "VGA controller"
                                    : "Display controller";
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

/*
 * Walks every bus/slot/function. If `print` is non-zero each device is
 * printed. Returns the number of devices found.
 */
static uint32_t pci_scan(int print)
{
    uint32_t count = 0;

    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            uint32_t first = pci_read(bus, slot, 0, 0);

            if ((first & 0xFFFF) == 0xFFFF)
                continue;                   /* nothing in this slot */

            /* Bit 7 of the header type says "this device has 8 functions". */
            uint32_t header = (pci_read(bus, slot, 0, 0x0C) >> 16) & 0xFF;
            uint32_t funcs = (header & 0x80) ? 8 : 1;

            for (uint32_t fn = 0; fn < funcs; fn++) {
                uint32_t id = pci_read(bus, slot, fn, 0);

                if ((id & 0xFFFF) == 0xFFFF)
                    continue;

                count++;

                if (!print)
                    continue;

                uint32_t classreg = pci_read(bus, slot, fn, 8);
                uint32_t cls = (classreg >> 24) & 0xFF;
                uint32_t sub = (classreg >> 16) & 0xFF;

                /* bus:slot.function  vendor:device  description */
                console_set_color(0x0B);
                console_put_hex_plain(bus, 2);
                console_putc(':');
                console_put_hex_plain(slot, 2);
                console_putc('.');
                console_put_dec(fn);
                console_write("  ");
                console_put_hex_plain(id & 0xFFFF, 4);
                console_putc(':');
                console_put_hex_plain(id >> 16, 4);
                console_write("  ");
                console_set_color(0x07);
                console_write(class_name(cls, sub));
                console_write(" - ");
                console_write(vendor_name(id & 0xFFFF));
                console_putc('\n');
            }
        }
    }

    return count;
}

uint32_t pci_count(void)
{
    return pci_scan(0);
}

void pci_list(void)
{
    uint32_t n = pci_scan(1);

    console_put_dec(n);
    console_write(" device(s) found\n");
}
