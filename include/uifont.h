/* uifont.h - the system typeface (myFont) for the whole desktop. */
#ifndef UIFONT_H
#define UIFONT_H
#include "types.h"
#include "gfx.h"

enum ui_weight { UI_THIN, UI_LIGHT, UI_REGULAR, UI_MEDIUM, UI_SEMIBOLD, UI_BOLD, UI_HEAVY, UI_BLACK, UI_MONO, UI_MONO_BOLD };

/* px is rounded to a baked size: 12, 14, 16, 20, 24 or 32. (x, y) is the top of the line box;
 * returns the x after the text. Anti-aliased, kerned, UTF-8, clipped to the surface. */
int  ui_text(struct gfx_surface *s, int x, int y, const char *utf8, uint32_t rgb, int px, int weight);
int  ui_text_width(const char *utf8, int px, int weight);
int  ui_text_height(int px, int weight);                /* line height */
int  ui_text_ascent(int px, int weight);
void ui_font_init(void);
#endif
