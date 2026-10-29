/* pic.h - the legacy 8259 interrupt controller. */
#ifndef PIC_H
#define PIC_H
#include "types.h"

void pic_init(void);                 /* remap IRQs to vectors 32..47 */
void pic_eoi(uint8_t irq);           /* tell the PIC we finished an IRQ */
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);

#endif
