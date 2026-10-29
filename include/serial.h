/* serial.h - COM1 serial port (used for logging; Bochs/QEMU can capture it). */
#ifndef SERIAL_H
#define SERIAL_H
#include "types.h"

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);
bool serial_has_data(void);
int  serial_getc(void);          /* -1 if no data */

#endif
