/*
 * cpu.c - CPU information using the CPUID instruction.
 *
 * CPUID is a CPU instruction: put a "leaf" number in EAX, run it, and the
 * CPU fills EAX, EBX, ECX and EDX with the answer.
 */

#include "hw.h"
#include "types.h"

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                  uint32_t *c, uint32_t *d)
{
    __asm__ volatile ("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(0));
}

/* Store the 4 bytes of a register as characters. */
static void reg_to_chars(uint32_t reg, char *out)
{
    for (int i = 0; i < 4; i++)
        out[i] = (char)(reg >> (i * 8));
}

/* Vendor string, e.g. "GenuineIntel" or "AuthenticAMD". */
void cpu_vendor(char out[13])
{
    uint32_t a, b, c, d;

    cpuid(0, &a, &b, &c, &d);

    /* The vendor string is stored in EBX, EDX, ECX (in that order). */
    reg_to_chars(b, &out[0]);
    reg_to_chars(d, &out[4]);
    reg_to_chars(c, &out[8]);
    out[12] = '\0';
}

/* Model name, e.g. "Intel(R) Core(TM) i7-...". Returns 0 if unsupported. */
int cpu_brand(char out[49])
{
    uint32_t a, b, c, d;

    cpuid(0x80000000, &a, &b, &c, &d);

    if (a < 0x80000004) {
        out[0] = '\0';
        return 0;
    }

    for (uint32_t leaf = 0; leaf < 3; leaf++) {
        cpuid(0x80000002 + leaf, &a, &b, &c, &d);

        reg_to_chars(a, &out[leaf * 16 + 0]);
        reg_to_chars(b, &out[leaf * 16 + 4]);
        reg_to_chars(c, &out[leaf * 16 + 8]);
        reg_to_chars(d, &out[leaf * 16 + 12]);
    }

    out[48] = '\0';
    return 1;
}

/* Family, model and stepping numbers. */
void cpu_signature(uint32_t *family, uint32_t *model, uint32_t *stepping)
{
    uint32_t a, b, c, d;

    cpuid(1, &a, &b, &c, &d);

    uint32_t base_family = (a >> 8) & 0xF;
    uint32_t base_model  = (a >> 4) & 0xF;

    *family   = base_family;
    *model    = base_model;
    *stepping = a & 0xF;

    if (base_family == 0xF)
        *family += (a >> 20) & 0xFF;

    if (base_family == 0x6 || base_family == 0xF)
        *model |= ((a >> 16) & 0xF) << 4;
}

/* Feature flag bits (leaf 1). */
uint32_t cpu_features_edx(void)
{
    uint32_t a, b, c, d;

    cpuid(1, &a, &b, &c, &d);
    return d;
}

uint32_t cpu_features_ecx(void)
{
    uint32_t a, b, c, d;

    cpuid(1, &a, &b, &c, &d);
    return c;
}

/*
 * Detects whether we are running inside a virtual machine. Returns 1 and
 * fills `out` with the hypervisor name (for example "KVMKVMKVM" or
 * "TCGTCGTCGTCG" for QEMU), or returns 0 on real hardware.
 */
int cpu_hypervisor(char out[13])
{
    uint32_t a, b, c, d;

    cpuid(1, &a, &b, &c, &d);

    if (!(c & (1u << 31))) {
        out[0] = '\0';
        return 0;
    }

    cpuid(0x40000000, &a, &b, &c, &d);

    reg_to_chars(b, &out[0]);
    reg_to_chars(c, &out[4]);
    reg_to_chars(d, &out[8]);
    out[12] = '\0';
    return 1;
}
