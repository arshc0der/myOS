/* camera.c (GUI) - "Camera": live preview of a video source, Take Photo saves a BMP in Pictures. */
#include "guiapi.h"
#include "camera.h"
#include "image.h"
#include "sys.h"
#include "vfs.h"
#include "heap.h"
#include "kprintf.h"
#include "kstring.h"

struct cam { int src; struct gfx_surface *frame; char status[96]; int shots; };

static void cam_open(struct gui_window *w)
{
    struct cam *c = kzalloc(sizeof *c);
    camera_init();
    w->priv = c;
    if (c) {
        c->frame = gfx_surface_create(320, 240);
        strcpy(c->status, camera_count() ? "Press Space to take a photo" : "No camera found");
    }
}

static void cam_close(struct gui_window *w)
{
    struct cam *c = w->priv;
    if (c) {
        gfx_surface_free(c->frame);
        kfree(c);
    }
}

static void shoot(struct cam *c)
{
    char p[VFS_PATH_MAX], dir[VFS_PATH_MAX];
    ksnprintf(dir, sizeof dir, "%s/Pictures", users_current() ? users_home() : "/tmp");
    struct vfs_stat st;
    if (vfs_stat(dir, &st) < 0)
        vfs_mkdir(dir);
    do
        ksnprintf(p, sizeof p, "%s/Photo %d.bmp", dir, ++c->shots);
    while (vfs_stat(p, &st) == 0);
    int rc = image_save_bmp(c->frame, p);
    if (rc < 0)
        ksnprintf(c->status, sizeof c->status, "Could not save the photo");
    else
        ksnprintf(c->status, sizeof c->status, "Saved %s", p);
}

static void cam_draw(struct gui_window *w)
{
    struct cam *c = w->priv;
    struct gfx_surface *s = w->surf;
    const struct gui_theme *t = gui_th;

    gfx_fill_rect(s, 0, 0, w->cw, w->ch, t->win);
    if (!c || !c->frame)
        return;
    struct camera *cam = camera_get(c->src);
    int vx = 20, vy = 52;
    if (cam && cam->grab && cam->w == c->frame->w) {
        cam->grab(cam, c->frame->px);
        gfx_blit(s, vx, vy, c->frame, 0, 0, c->frame->w, c->frame->h);
    } else {
        gfx_fill_rect(s, vx, vy, 320, 240, GFX_RGB(20, 20, 22));
        gui_label_center(s, vx, vy + 110, 320, "No picture", GFX_RGB(180, 180, 186));
    }
    gfx_rect(s, vx - 1, vy - 1, 322, 242, t->line);
    gfx_text(s, 20, 18, "Camera", t->text, GFX_TRANSPARENT, 2);
    gfx_text(s, 20, 36, cam ? cam->name : "no source", t->dim, GFX_TRANSPARENT, 1);
    gui_button(s, 20, 304, 130, 30, "Take Photo", true);
    if (camera_count() > 1)
        gui_button(s, 160, 304, 130, 30, "Next Camera", false);
    gfx_text(s, 20, 346, c->status, t->dim, GFX_TRANSPARENT, 1);
    gfx_text(s, 20, 366, "USB and built-in webcams: UVC driver (USB 3 ports only).", t->dim, GFX_TRANSPARENT, 1);
}

static void cam_click(struct gui_window *w, int x, int y, int b)
{
    struct cam *c = w->priv;
    (void)b;
    if (!c) return;
    if (gui_hit(x, y, 20, 304, 130, 30))
        shoot(c);
    else if (camera_count() > 1 && gui_hit(x, y, 160, 304, 130, 30))
        c->src = (c->src + 1) % camera_count();
    gui_redraw();
}

static void cam_key(struct gui_window *w, int key, int mods)
{
    (void)mods;
    if (key == ' ' && w->priv)
        shoot(w->priv);
    gui_redraw();
}

static void cam_tick(struct gui_window *w) { (void)w; gui_redraw(); }

static void cam_icon(struct gfx_surface *s, int x, int y, int sz, uint32_t g, uint32_t tile)
{
#define P(f) ((sz * (f)) / 100)
    gfx_fill_round_rect(s, x + P(16), y + P(30), P(68), P(44), P(8), g);
    gfx_fill_round_rect(s, x + P(34), y + P(22), P(24), P(12), P(4), g);
    gfx_fill_circle(s, x + P(50), y + P(52), P(15), tile);
    gfx_fill_circle(s, x + P(50), y + P(52), P(9), g);
#undef P
}

static const struct gui_app camera_app = {
    .name = "Camera", .title = "Camera", .cw = 360, .ch = 390, .single = true, .dock = 55,
    .open = cam_open, .close = cam_close, .draw = cam_draw, .click = cam_click, .key = cam_key,
    .tick = cam_tick, .icon = cam_icon,
};
GUI_APP(camera_app);
