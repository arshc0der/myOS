/*
 * types.h - basic types and helper macros used everywhere in the kernel.
 */
#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

typedef int32_t  ssize_t;
typedef uint32_t paddr_t;       /* a physical (real RAM) address  */
typedef uint32_t vaddr_t;       /* a virtual (CPU-visible) address */

#define PAGE_SIZE        4096u
#define PAGE_SHIFT       12

#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((a) - 1))
#define PAGE_ALIGN_DOWN(x) ALIGN_DOWN((x), PAGE_SIZE)
#define PAGE_ALIGN_UP(x)   ALIGN_UP((x), PAGE_SIZE)

#define ARRAY_SIZE(a)    (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)        ((a) < (b) ? (a) : (b))
#define MAX(a, b)        ((a) > (b) ? (a) : (b))
#define BIT(n)           (1u << (n))
#define KiB(n)           ((n) * 1024u)
#define MiB(n)           ((n) * 1024u * 1024u)

#define PACKED           __attribute__((packed))
#define NORETURN         __attribute__((noreturn))
#define UNUSED           __attribute__((unused))
#define ALIGNED(n)       __attribute__((aligned(n)))
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))

/* Given a pointer to a member, get a pointer to the struct that contains it. */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#endif
