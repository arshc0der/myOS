/* gdt.h - segment descriptors and the task-state segment (TSS). */
#ifndef GDT_H
#define GDT_H
#include "types.h"

#define SEG_KCODE 0x08
#define SEG_KDATA 0x10
#define SEG_UCODE 0x1B      /* 0x18 | ring 3 */
#define SEG_UDATA 0x23      /* 0x20 | ring 3 */

void gdt_init(void);
/* Stack the CPU switches to when an interrupt arrives from ring 3. */
void tss_set_kernel_stack(uint32_t esp0);

#endif
