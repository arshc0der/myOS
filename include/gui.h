/*
 * gui.h - the graphical desktop.
 *
 * gui_run() shows a splash screen with the MyOS logo, then a desktop with
 * windows (Terminal, Files, System Info, Notepad, About), a taskbar and a
 * start menu. It works with the mouse, and also fully from the keyboard:
 *
 *   Alt+T Terminal   Alt+F Files   Alt+I System info   Alt+N Notepad
 *   Alt+Tab next window   Alt+W close window   Alt+M start menu   Alt+Q quit
 *
 * Start it from the boot menu ("graphical desktop") or with the `gui`
 * shell command. gui_run() returns when the user quits the desktop.
 */
#ifndef GUI_H
#define GUI_H
#include "types.h"

bool gui_supported(void);       /* is there a graphics framebuffer? */
void gui_run(void);             /* blocks until the desktop is closed */
void gui_splash(void);          /* just the logo screen */

#endif
