/*
 * uifont.c - myFont as the system font. Glue between the freestanding renderer (myfont.c) and our surfaces.
 * The 10 faces are embedded by fonts_data.S. Every proportional face falls back to myFont Mono for
 * code points it does not have.
 */
#include "uifont.h"
#include "myfont.h"
#include "kprintf.h"
#include "kstring.h"

extern const uint8_t myfont_thin_start[], myfont_thin_end[];
extern const uint8_t myfont_light_start[], myfont_light_end[];
extern const uint8_t myfont_regular_start[], myfont_regular_end[];
extern const uint8_t myfont_medium_start[], myfont_medium_end[];
extern const uint8_t myfont_semibold_start[], myfont_semibold_end[];
extern const uint8_t myfont_bold_start[], myfont_bold_end[];
extern const uint8_t myfont_heavy_start[], myfont_heavy_end[];
extern const uint8_t myfont_black_start[], myfont_black_end[];
extern const uint8_t myfont_mono_start[], myfont_mono_end[];
extern const uint8_t myfont_monobold_start[], myfont_monobold_end[];

static myfont_face faces[10];
static bool ready, ok;

void ui_font_init(void)
{
    if (ready)
        return;
    ready = true;
    ok = true;
    const uint8_t *b[10] = { myfont_thin_start, myfont_light_start, myfont_regular_start, myfont_medium_start, myfont_semibold_start,
                             myfont_bold_start, myfont_heavy_start, myfont_black_start, myfont_mono_start, myfont_monobold_start };
    const uint8_t *e[10] = { myfont_thin_end, myfont_light_end, myfont_regular_end, myfont_medium_end, myfont_semibold_end,
                             myfont_bold_end, myfont_heavy_end, myfont_black_end, myfont_mono_end, myfont_monobold_end };
    for (int i = 0; i < 10; i++)
        if (myfont_init(&faces[i], b[i], (size_t)(e[i] - b[i])) < 0)
            ok = false;
    if (ok)
        for (int i = 0; i < 8; i++)
            myfont_set_fallback(&faces[i], &faces[i == UI_BOLD || i == UI_HEAVY || i == UI_BLACK ? UI_MONO_BOLD : UI_MONO]);
    else
        kprintf("uifont: a font file is damaged, using the 8x8 bitmap font\n");
}

static const myfont_face *face(int weight)
{
    ui_font_init();
    if (!ok)
        return NULL;
    return &faces[weight < 0 || weight > 9 ? UI_REGULAR : weight];
}

int ui_text_width(const char *t, int px, int weight)
{
    const myfont_face *f = face(weight);
    return f ? myfont_text_width(f, myfont_size_index(f, px), t) : (int)strlen(t) * 8;
}

int ui_text_ascent(int px, int weight)
{
    const myfont_face *f = face(weight);
    return f ? myfont_ascent(f, myfont_size_index(f, px)) : 8;
}

int ui_text_height(int px, int weight)
{
    const myfont_face *f = face(weight);
    return f ? myfont_line_height(f, myfont_size_index(f, px)) : 8;
}

int ui_text(struct gfx_surface *s, int x, int y, const char *t, uint32_t rgb, int px, int weight)
{
    const myfont_face *f = face(weight);
    if (!f || !s || !s->px)
        return x;
    myfont_surface ms = { s->px, s->w, s->h, s->stride * 4 };
    int idx = myfont_size_index(f, px);
    return myfont_draw_text(&ms, f, idx, x, y + myfont_ascent(f, idx), rgb, t);
}
