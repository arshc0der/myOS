/*
 * powerinfo.c - is this a laptop? Reads the ACPI tables: RSDP -> RSDT -> FADT (power profile) and looks for a
 * battery device ("PNP0C0A") in the DSDT/SSDTs. It does NOT read the charge level: that needs the ACPI machine
 * language (AML) interpreter and the embedded controller, neither of which MyOS has.
 */
#include "notify.h"
#include "vmm.h"
#include "kstring.h"
#include "log.h"

static struct power_info pi;

static const uint8_t *phys(uint32_t addr, uint32_t len)
{
    uint32_t first = addr & ~0xFFFu, last = (addr + len + 0xFFF) & ~0xFFFu;
    for (uint32_t p = first; p < last; p += 0x1000)
        if (!vmm_is_mapped(p) && vmm_map_range(p, p, 0x1000, 0) < 0)
            return NULL;
    return (const uint8_t *)addr;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static bool has_battery(const uint8_t *t, uint32_t len)
{
    for (uint32_t i = 0; i + 7 <= len; i++)
        if (!memcmp(t + i, "PNP0C0A", 7))
            return true;
    return false;
}

static bool scan_table(uint32_t addr)
{
    const uint8_t *t = phys(addr, 36);
    if (!t)
        return false;
    uint32_t len = rd32(t + 4);
    if (len < 36 || len > 0x200000)
        return false;
    t = phys(addr, len);
    return t && has_battery(t, len);
}

const struct power_info *power_probe(void)
{
    if (pi.probed)
        return &pi;
    pi.probed = true;
    const uint8_t *r = NULL;
    for (uint32_t a = 0xE0000; a < 0x100000 && !r; a += 16) {
        const uint8_t *c = phys(a, 16);
        if (c && !memcmp(c, "RSD PTR ", 8))
            r = c;
    }
    if (!r)
        return &pi;
    uint32_t rsdt = rd32(r + 16);
    const uint8_t *t = phys(rsdt, 36);
    if (!t || memcmp(t, "RSDT", 4))
        return &pi;
    uint32_t len = rd32(t + 4);
    if (len < 36 || len > 0x10000)
        return &pi;
    t = phys(rsdt, len);
    if (!t)
        return &pi;
    pi.acpi = true;
    for (uint32_t i = 36; i + 4 <= len; i += 4) {
        uint32_t a = rd32(t + i);
        const uint8_t *h = phys(a, 36);
        if (!h)
            continue;
        if (!memcmp(h, "FACP", 4)) {
            uint32_t fl = rd32(h + 4);
            if (fl > 45) {
                uint8_t prof = h[45];                   /* preferred PM profile: 2 = mobile (laptop) */
                pi.mobile = prof == 2 || prof == 8;
            }
            uint32_t dsdt = fl > 44 ? rd32(h + 40) : 0;
            if (dsdt && scan_table(dsdt))
                pi.battery = true;
        } else if (!memcmp(h, "SSDT", 4) && scan_table(a))
            pi.battery = true;
    }
    if (pi.battery)
        pi.mobile = true;
    log_info("power", "ACPI %s, %s, battery %s", pi.acpi ? "found" : "not found", pi.mobile ? "laptop profile" : "desktop profile", pi.battery ? "present" : "not found");
    return &pi;
}
