/* selftest_gfx.c - tests for drawing code, the text grid and the framebuffer. */
#include "kernel.h"
#include "gfx.h"
#include "tgrid.h"
#include "heap.h"
#include "kstring.h"
#include "serial.h"
#include "multiboot.h"
#include "kprintf.h"

#define T TEST_CHECK

/* Print the display as a PPM image over the serial port (every 2nd pixel)
 * so a script can turn it into a picture:  FBDUMP-BEGIN w h ... FBDUMP-END */
void gfx_dump_serial(void)
{
    int k = boot_has_arg("fbfull") ? 1 : 2;
    int w = gfx_width() / k, h = gfx_height() / k;
    static const char hex[] = "0123456789abcdef";
    static char row[8192];

    serial_write("\nFBDUMP-BEGIN ");
    char hdr[32];
    ksnprintf(hdr, sizeof hdr, "%d %d\n", w, h);
    serial_write(hdr);
    for (int y = 0; y < h; y++) {
        int n = 0;
        for (int x = 0; x < w && n + 8 < (int)sizeof row; x++) {
            uint32_t c = gfx_display_pixel(x * k, y * k);
            for (int s = 20; s >= 0; s -= 4)
                row[n++] = hex[(c >> s) & 15];
        }
        row[n++] = '\n';
        row[n] = '\0';
        serial_write(row);
    }
    serial_write("FBDUMP-END\n");
}

void selftest_gfx(void)
{
    /* ---- primitives on a memory surface ---- */
    static uint32_t mem[64 * 64];
    struct gfx_surface s;

    gfx_surface_init(&s, mem, 64, 64);
    gfx_fill_rect(&s, 0, 0, 64, 64, GFX_BLACK);
    gfx_fill_rect(&s, 10, 10, 20, 10, GFX_RGB(255, 0, 0));
    T(gfx_get_pixel(&s, 10, 10) == GFX_RGB(255, 0, 0) && gfx_get_pixel(&s, 29, 19) == GFX_RGB(255, 0, 0) &&
      gfx_get_pixel(&s, 30, 19) == GFX_BLACK && gfx_get_pixel(&s, 9, 10) == GFX_BLACK,
      "fill_rect covers exactly its area");
    gfx_fill_rect(&s, -10, -10, 15, 15, GFX_WHITE);
    gfx_fill_rect(&s, 60, 60, 100, 100, GFX_WHITE);
    T(gfx_get_pixel(&s, 0, 0) == GFX_WHITE && gfx_get_pixel(&s, 4, 4) == GFX_WHITE &&
      gfx_get_pixel(&s, 5, 5) == GFX_BLACK && gfx_get_pixel(&s, 63, 63) == GFX_WHITE,
      "drawing is clipped at the edges");
    gfx_fill_rect(&s, 0, 0, 64, 64, GFX_BLACK);
    gfx_rect(&s, 5, 5, 10, 8, GFX_WHITE);
    T(gfx_get_pixel(&s, 5, 5) == GFX_WHITE && gfx_get_pixel(&s, 14, 12) == GFX_WHITE &&
      gfx_get_pixel(&s, 10, 8) == GFX_BLACK, "rect outline");
    gfx_fill_rect(&s, 0, 0, 64, 64, GFX_BLACK);
    gfx_line(&s, 0, 0, 40, 40, GFX_WHITE);
    T(gfx_get_pixel(&s, 20, 20) == GFX_WHITE && gfx_get_pixel(&s, 40, 40) == GFX_WHITE &&
      gfx_get_pixel(&s, 20, 21) == GFX_BLACK, "diagonal line");
    gfx_fill_rect(&s, 0, 0, 64, 64, GFX_BLACK);
    gfx_fill_circle(&s, 32, 32, 10, GFX_WHITE);
    T(gfx_get_pixel(&s, 32, 32) == GFX_WHITE && gfx_get_pixel(&s, 32, 22) == GFX_WHITE &&
      gfx_get_pixel(&s, 22, 22) == GFX_BLACK, "filled circle");
    gfx_fill_rect(&s, 0, 0, 64, 64, GFX_BLACK);
    gfx_fill_round_rect(&s, 8, 8, 40, 30, 8, GFX_WHITE);
    T(gfx_get_pixel(&s, 28, 20) == GFX_WHITE && gfx_get_pixel(&s, 8, 8) == GFX_BLACK &&
      gfx_get_pixel(&s, 8, 22) == GFX_WHITE, "rounded rectangle has round corners");

    uint32_t m2[16 * 16];
    struct gfx_surface s2;
    gfx_surface_init(&s2, m2, 16, 16);
    gfx_fill_rect(&s2, 0, 0, 16, 16, GFX_RGB(0, 0, 255));
    gfx_fill_rect(&s, 0, 0, 64, 64, GFX_BLACK);
    gfx_blit(&s, 60, 60, &s2, 0, 0, 16, 16);
    T(gfx_get_pixel(&s, 63, 63) == GFX_RGB(0, 0, 255) && gfx_get_pixel(&s, 59, 59) == GFX_BLACK,
      "blit is clipped");
    gfx_blit(&s, -8, -8, &s2, 0, 0, 16, 16);
    T(gfx_get_pixel(&s, 0, 0) == GFX_RGB(0, 0, 255) && gfx_get_pixel(&s, 8, 8) == GFX_BLACK,
      "blit with negative position");

    uint32_t b = gfx_blend(GFX_BLACK, GFX_WHITE, 128);
    T(((b >> 16) & 255) >= 127 && ((b >> 16) & 255) <= 129, "alpha blend halfway");
    gfx_gradient_v(&s, 0, 0, 10, 11, GFX_BLACK, GFX_WHITE);
    T(gfx_get_pixel(&s, 0, 0) == GFX_BLACK && gfx_get_pixel(&s, 0, 10) == GFX_WHITE, "vertical gradient");

    gfx_fill_rect(&s, 0, 0, 64, 64, GFX_BLACK);
    gfx_char(&s, 0, 0, 'A', GFX_WHITE, GFX_TRANSPARENT, 1);
    int lit = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            lit += gfx_get_pixel(&s, x, y) == GFX_WHITE;
    T(lit >= 12 && lit <= 40, "font draws the letter A");
    T(gfx_text_width("abc", 2) == 48, "text width");

    gfx_fill_rect(&s, 0, 0, 64, 64, GFX_BLACK);
    gfx_draw_logo(&s, 0, 0, 64);
    T(gfx_get_pixel(&s, 32, 32) != GFX_BLACK && gfx_get_pixel(&s, 1, 1) == GFX_BLACK,
      "logo draws a rounded tile");

    /* ---- text grid ---- */
    struct tgrid tg;
    T(tg_init(&tg, 10, 4, 20), "tgrid init");
    for (const char *p = "hello"; *p; p++)
        tg_putc(&tg, *p);
    T(tg_cell(&tg, 0, 0)->ch == 'h' && tg_cell(&tg, 0, 4)->ch == 'o' && tg.col == 5, "tgrid stores text");
    tg_putc(&tg, '\n');
    T(tg.row == 1 && tg.col == 0, "tgrid newline");
    for (const char *p = "0123456789AB"; *p; p++)
        tg_putc(&tg, *p);
    T(tg.row == 2 && tg_cell(&tg, 2, 1)->ch == 'B', "tgrid wraps long lines");
    tg_putc(&tg, '\b');
    T(tg.col == 1, "tgrid backspace");
    for (int i = 0; i < 10; i++)
        tg_putc(&tg, '\n');
    T(tg.above > 0 && tg.row == 3, "tgrid scrolls and keeps history");
    tg_scroll_view(&tg, 100);
    T(tg.view == tg.above, "scrollback view is limited to the history");
    tg_scroll_view(&tg, -1000);
    T(tg.view == 0, "scrolling forward stops at the live screen");
    tg_clear(&tg);
    T(tg_cell(&tg, 0, 0)->ch == ' ' && tg.row == 0, "tgrid clear");
    tg_free(&tg);

    /* ---- real framebuffer (only when the machine has one) ---- */
    if (gfx_available()) {
        struct gfx_surface *sc = gfx_screen();
        int w = gfx_width(), h = gfx_height();

        T(w >= 320 && h >= 200, "framebuffer has a sensible size");
        gfx_fill_rect(sc, 0, 0, w, h, GFX_RGB(200, 30, 60));
        gfx_fill_rect(sc, 5, 5, 3, 3, GFX_RGB(10, 220, 90));
        gfx_flush_all();
        uint32_t p1 = gfx_display_pixel(w / 2, h / 2), p2 = gfx_display_pixel(6, 6);
        /* 16-bit displays lose low bits, so compare loosely */
        T(((p1 >> 16) & 255) > 190 && ((p1 >> 8) & 255) < 50 && (p1 & 255) > 40 && (p1 & 255) < 80,
          "flush copies colours to the display (centre pixel)");
        T(((p2 >> 8) & 255) > 200 && ((p2 >> 16) & 255) < 30, "flush copies colours (small square)");
        if (boot_has_arg("fbdump")) {
            /* nothing to show yet; the caller takes the picture later */
        }
    }
}
