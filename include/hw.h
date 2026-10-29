/* hw.h - hardware information: CPU, PCI bus, real-time clock. */
#ifndef HW_H
#define HW_H
#include "types.h"

/* CPU (src/drivers/cpu.c) */
void cpu_vendor(char out[13]);
int  cpu_brand(char out[49]);                   /* 0 if the CPU has no name */
void cpu_signature(uint32_t *family, uint32_t *model, uint32_t *stepping);
uint32_t cpu_features_edx(void);
uint32_t cpu_features_ecx(void);
int  cpu_hypervisor(char out[13]);              /* 1 inside a virtual machine */

/* PCI (src/drivers/pci.c) */
struct pci_device {
    uint8_t bus, slot, func;
    uint16_t vendor, device;
    uint8_t cls, sub;
};
int  pci_scan(struct pci_device *out, int max); /* returns number found */
const char *pci_vendor_name(uint16_t vendor);
const char *pci_class_name(uint8_t cls, uint8_t sub);

/* Real-time clock (src/drivers/rtc.c). t = year, month, day, hour, min, sec */
void rtc_read(uint32_t t[6]);

#endif
