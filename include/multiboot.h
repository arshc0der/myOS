/*
 * multiboot.h - what GRUB tells us about the machine.
 *
 * GRUB passes a pointer to a "Multiboot information" structure. We read it
 * once at boot (boot_info_init) and keep a friendly copy in `boot_info`.
 */
#ifndef MULTIBOOT_H
#define MULTIBOOT_H
#include "types.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

#define MB_MEM_AVAILABLE 1
#define MB_MEM_RESERVED  2

struct boot_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
};

#define BOOT_MAX_MMAP 64
#define BOOT_MAX_MODS 8

struct boot_module {
    paddr_t start, end;
    char    name[32];
};

struct boot_fb {
    bool     present;
    bool     is_text;           /* true: EGA text mode, false: pixels */
    uint64_t addr;
    uint32_t pitch, width, height, bpp;
    uint8_t  red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
};

struct boot_info {
    bool     valid;
    uint32_t mem_lower_kb, mem_upper_kb;
    char     loader[48];
    char     cmdline[256];
    int      mmap_count;
    struct boot_mmap_entry mmap[BOOT_MAX_MMAP];
    int      module_count;
    struct boot_module modules[BOOT_MAX_MODS];
    struct boot_fb fb;
    /* memory used by the boot structures themselves (never handed out) */
    int      reserved_count;
    struct { paddr_t start, end; } reserved[24];
};

extern struct boot_info boot_info;

void boot_info_init(uint32_t magic, uint32_t mbi_addr);
/* Does the kernel command line contain this word? (set in grub.cfg) */
bool boot_has_arg(const char *word);
/* Value of key=value on the command line, or NULL. */
const char *boot_arg_value(const char *key, char *buf, size_t size);
const char *boot_mem_type_name(uint32_t type);

#endif
