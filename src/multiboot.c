/*
 * multiboot.c - reads the information GRUB gives the kernel at boot.
 *
 * GRUB passes a pointer to a "Multiboot info" structure. It tells us how
 * much memory the machine has, a map of every memory region, the name of
 * the bootloader and more. Each field is only valid if its bit is set in
 * `flags`.
 */

#include <stdint.h>

#define MB_MAGIC 0x2BADB002

#define MB_FLAG_MEM     (1u << 0)   /* mem_lower / mem_upper valid   */
#define MB_FLAG_MMAP    (1u << 6)   /* memory map valid              */
#define MB_FLAG_LOADER  (1u << 9)   /* boot_loader_name valid        */

struct mb_info {
    uint32_t flags;
    uint32_t mem_lower;         /* KiB below 1 MiB */
    uint32_t mem_upper;         /* KiB above 1 MiB */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;  /* address of a C string */
} __attribute__((packed));

struct mb_mmap_entry {
    uint32_t size;              /* size of the rest of this entry */
    uint64_t base;
    uint64_t length;
    uint32_t type;              /* 1 = usable RAM */
} __attribute__((packed));

static const struct mb_info *info;

void multiboot_init(uint32_t magic, uint32_t addr)
{
    if (magic == MB_MAGIC)
        info = (const struct mb_info *)(uintptr_t)addr;
}

int boot_memory_available(void)
{
    return info != 0 && (info->flags & MB_FLAG_MEM);
}

uint32_t mb_mem_lower_kb(void)
{
    return boot_memory_available() ? info->mem_lower : 0;
}

uint32_t mb_mem_upper_kb(void)
{
    return boot_memory_available() ? info->mem_upper : 0;
}

uint32_t boot_memory_kb(void)
{
    return mb_mem_lower_kb() + mb_mem_upper_kb();
}

/* Name of the bootloader (for example "GRUB 2.12"), or 0 if unknown. */
const char *mb_bootloader_name(void)
{
    if (info && (info->flags & MB_FLAG_LOADER) && info->boot_loader_name)
        return (const char *)(uintptr_t)info->boot_loader_name;

    return 0;
}

/*
 * Memory map entries are variable-sized. Each entry's `size` field does
 * not count itself, so the next entry starts at (this + size + 4).
 */
static const struct mb_mmap_entry *mmap_at(uint32_t index)
{
    if (!info || !(info->flags & MB_FLAG_MMAP))
        return 0;

    uintptr_t addr = info->mmap_addr;
    uintptr_t end  = addr + info->mmap_length;
    uint32_t i = 0;

    while (addr < end) {
        const struct mb_mmap_entry *e =
            (const struct mb_mmap_entry *)addr;

        if (e->size < 20)           /* malformed entry: stop */
            break;

        if (i == index)
            return e;

        addr += e->size + 4;
        i++;
    }

    return 0;
}

uint32_t mb_mmap_count(void)
{
    uint32_t n = 0;

    while (mmap_at(n))
        n++;

    return n;
}

int mb_mmap_get(uint32_t index, uint64_t *base,
                uint64_t *length, uint32_t *type)
{
    const struct mb_mmap_entry *e = mmap_at(index);

    if (!e)
        return 0;

    *base   = e->base;
    *length = e->length;
    *type   = e->type;
    return 1;
}

const char *mb_type_name(uint32_t type)
{
    switch (type) {
    case 1:  return "usable";
    case 2:  return "reserved";
    case 3:  return "ACPI reclaimable";
    case 4:  return "ACPI NVS";
    case 5:  return "bad RAM";
    default: return "unknown";
    }
}
