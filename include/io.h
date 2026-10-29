/*
 * io.h - CPU port I/O and interrupt-flag helpers.
 *
 * Many devices are controlled by reading and writing numbered "ports"
 * with the in/out instructions. These small inline functions wrap them.
 */
#ifndef IO_H
#define IO_H

#include "types.h"

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t v)
{
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(uint16_t port, uint32_t v)
{
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Read/write many 16-bit words at once (used for disk sector transfers). */
static inline void insw(uint16_t port, void *buf, uint32_t count)
{
    __asm__ volatile ("cld; rep insw"
                      : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}

static inline void outsw(uint16_t port, const void *buf, uint32_t count)
{
    __asm__ volatile ("cld; rep outsw"
                      : "+S"(buf), "+c"(count) : "d"(port));
}

/* A tiny delay: writing to an unused port takes about a microsecond. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* ---- interrupt flag ---- */

static inline void cpu_cli(void) { __asm__ volatile ("cli" : : : "memory"); }
static inline void cpu_sti(void) { __asm__ volatile ("sti" : : : "memory"); }
static inline void cpu_hlt(void) { __asm__ volatile ("hlt" : : : "memory"); }
/* Enable interrupts and halt until the next one (no race in between). */
static inline void cpu_hlt_wait(void) { __asm__ volatile ("sti; hlt" : : : "memory"); }

/* Disable interrupts and return the previous EFLAGS (to restore later). */
static inline uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    __asm__ volatile ("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

static inline bool irq_enabled(void)
{
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0" : "=r"(flags));
    return (flags & 0x200) != 0;
}

/* ---- control registers and misc ---- */

static inline uint32_t read_cr0(void)
{
    uint32_t v; __asm__ volatile ("movl %%cr0, %0" : "=r"(v)); return v;
}
static inline void write_cr0(uint32_t v)
{
    __asm__ volatile ("movl %0, %%cr0" : : "r"(v) : "memory");
}
static inline uint32_t read_cr2(void)
{
    uint32_t v; __asm__ volatile ("movl %%cr2, %0" : "=r"(v)); return v;
}
static inline uint32_t read_cr3(void)
{
    uint32_t v; __asm__ volatile ("movl %%cr3, %0" : "=r"(v)); return v;
}
static inline void write_cr3(uint32_t v)
{
    __asm__ volatile ("movl %0, %%cr3" : : "r"(v) : "memory");
}
static inline uint32_t read_cr4(void)
{
    uint32_t v; __asm__ volatile ("movl %%cr4, %0" : "=r"(v)); return v;
}
static inline void write_cr4(uint32_t v)
{
    __asm__ volatile ("movl %0, %%cr4" : : "r"(v) : "memory");
}

/* Invalidate the CPU's cached translation for one page. */
static inline void invlpg(uint32_t vaddr)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#endif
