/* panels.h - Control Center, Notification Center (with calendar) and notification banners. */
#ifndef PANELS_H
#define PANELS_H
#include "gfx.h"

enum { PANEL_NONE, PANEL_CONTROL, PANEL_NOTIFY };

void panels_init(void);                              /* after login: apply saved radio / brightness settings, reminders */
void panels_toggle(int which);
bool panels_is_open(void);
void panels_draw(struct gfx_surface *s);             /* every frame: panel, banners, brightness easing */
bool panels_mouse(int x, int y, uint8_t buttons, uint8_t prev);   /* true = event used */
bool panels_key(int key);                            /* true = key used */
void panels_apply_radios(void);                      /* Wi-Fi / Bluetooth follow Airplane Mode */
void gui_request_frames(void);                       /* keep rendering (animation running) */
#endif
