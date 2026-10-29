/* multiboot.c - parse the Multiboot v1 information structure. */
#include "multiboot.h"
#include "kstring.h"
#include "log.h"

struct boot_info boot_info;

struct mb_info {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
    uint32_t drives_length, drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info, vbe_mode_info;
    uint16_t vbe_mode, vbe_iface_seg, vbe_iface_off, vbe_iface_len;
    uint64_t fb_addr;
    uint32_t fb_pitch, fb_width, fb_height;
    uint8_t  fb_bpp, fb_type;
    uint8_t  color_info[6];
} PACKED;

struct mb_mmap { uint32_t size; uint64_t base, len; uint32_t type; } PACKED;
struct mb_mod  { uint32_t start, end, string, reserved; } PACKED;

#define F_MEM    (1u << 0)
#define F_CMD    (1u << 2)
#define F_MODS   (1u << 3)
#define F_MMAP   (1u << 6)
#define F_LOADER (1u << 9)
#define F_FB     (1u << 12)

/* Remember a range of memory used by the boot structures. */
static void note(paddr_t start, size_t len)
{
    if (len == 0 || boot_info.reserved_count >= 24)
        return;
    boot_info.reserved[boot_info.reserved_count].start = start;
    boot_info.reserved[boot_info.reserved_count].end = start + (paddr_t)len;
    boot_info.reserved_count++;
}

void boot_info_init(uint32_t magic, uint32_t addr)
{
    memset(&boot_info, 0, sizeof boot_info);
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC || addr == 0) {
        strlcpy(boot_info.loader, "unknown", sizeof boot_info.loader);
        return;
    }

    const struct mb_info *m = (const struct mb_info *)addr;
    boot_info.valid = true;
    note(addr, sizeof *m);

    if (m->flags & F_MEM) {
        boot_info.mem_lower_kb = m->mem_lower;
        boot_info.mem_upper_kb = m->mem_upper;
    }
    if (m->flags & F_LOADER) {
        strlcpy(boot_info.loader, (const char *)m->boot_loader_name,
                sizeof boot_info.loader);
        note(m->boot_loader_name, strlen((const char *)m->boot_loader_name) + 1);
    }
    if (m->flags & F_CMD) {
        strlcpy(boot_info.cmdline, (const char *)m->cmdline,
                sizeof boot_info.cmdline);
        note(m->cmdline, strlen((const char *)m->cmdline) + 1);
    }
    if (m->flags & F_MMAP) {
        uint32_t p = m->mmap_addr, end = m->mmap_addr + m->mmap_length;
        note(m->mmap_addr, m->mmap_length);
        while (p < end && boot_info.mmap_count < BOOT_MAX_MMAP) {
            const struct mb_mmap *e = (const struct mb_mmap *)p;
            struct boot_mmap_entry *o = &boot_info.mmap[boot_info.mmap_count++];
            o->base = e->base;
            o->length = e->len;
            o->type = e->type;
            p += e->size + 4;
        }
    }
    if (m->flags & F_MODS) {
        const struct mb_mod *md = (const struct mb_mod *)m->mods_addr;
        note(m->mods_addr, m->mods_count * sizeof *md);
        for (uint32_t i = 0; i < m->mods_count && i < BOOT_MAX_MODS; i++) {
            struct boot_module *o = &boot_info.modules[boot_info.module_count++];
            o->start = md[i].start;
            o->end = md[i].end;
            if (md[i].string)
                strlcpy(o->name, (const char *)md[i].string, sizeof o->name);
            note(md[i].start, md[i].end - md[i].start);
        }
    }
    if (m->flags & F_FB) {
        struct boot_fb *f = &boot_info.fb;
        f->present = true;
        f->is_text = (m->fb_type == 2);
        f->addr = m->fb_addr;
        f->pitch = m->fb_pitch;
        f->width = m->fb_width;
        f->height = m->fb_height;
        f->bpp = m->fb_bpp;
        if (m->fb_type == 1) {
            f->red_pos = m->color_info[0];   f->red_size = m->color_info[1];
            f->green_pos = m->color_info[2]; f->green_size = m->color_info[3];
            f->blue_pos = m->color_info[4];  f->blue_size = m->color_info[5];
        }
    }
}

bool boot_has_arg(const char *word)
{
    size_t n = strlen(word);
    const char *p = boot_info.cmdline;

    while (*p) {
        while (*p == ' ')
            p++;
        const char *s = p;
        while (*p && *p != ' ')
            p++;
        if ((size_t)(p - s) == n && strncmp(s, word, n) == 0)
            return true;
    }
    return false;
}

const char *boot_arg_value(const char *key, char *buf, size_t size)
{
    size_t n = strlen(key);
    const char *p = boot_info.cmdline;

    while (*p) {
        while (*p == ' ')
            p++;
        const char *s = p;
        while (*p && *p != ' ')
            p++;
        if ((size_t)(p - s) > n && strncmp(s, key, n) == 0 && s[n] == '=') {
            size_t len = (size_t)(p - s) - n - 1;
            if (len >= size)
                len = size - 1;
            memcpy(buf, s + n + 1, len);
            buf[len] = '\0';
            return buf;
        }
    }
    return NULL;
}

const char *boot_mem_type_name(uint32_t t)
{
    switch (t) {
    case 1: return "usable";
    case 2: return "reserved";
    case 3: return "ACPI reclaimable";
    case 4: return "ACPI NVS";
    case 5: return "bad";
    default: return "unknown";
    }
}
