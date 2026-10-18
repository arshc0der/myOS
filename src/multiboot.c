
#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

static uint32_t boot_magic;
static uint32_t boot_info_addr;

void multiboot_init(uint32_t magic, uint32_t addr)
{
    boot_magic = magic;
    boot_info_addr = addr;
}

int boot_memory_available(void)
{
    if (boot_magic != MULTIBOOT_BOOTLOADER_MAGIC)
        return 0;

    if (boot_info_addr == 0)
        return 0;

    /* Multiboot info flag bit 0 indicates memory fields are valid. */
    const uint32_t *info = (const uint32_t *)boot_info_addr;
    return (info[0] & 1u) != 0;
}

uint32_t boot_memory_kb(void)
{
    if (!boot_memory_available())
        return 0;

    const uint32_t *info = (const uint32_t *)boot_info_addr;

    /* mem_upper is at byte offset 8 in the Multiboot v1 info structure.
       It reports KiB above the first 1 MiB. */
    return info[2] + 1024u;
}
