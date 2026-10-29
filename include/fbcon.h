/* fbcon.h - text console drawn on the graphics framebuffer. */
#ifndef FBCON_H
#define FBCON_H
#include "console.h"

extern const struct console_backend fbcon_console;
bool fbcon_available(void);
/* Size of the text grid (for the shell) */
int  fbcon_cols(void);
int  fbcon_rows(void);

#endif
