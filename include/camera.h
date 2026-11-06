/*
 * camera.h - video sources for the Camera app.
 *
 * A camera driver fills a struct camera and registers it. grab() writes one frame
 * of w*h pixels (0x00RRGGBB) into `px`. MyOS ships one built-in source, an animated
 * test pattern, so the Camera app works in any emulator. USB and built-in webcams are registered by
 * src/drivers/usb/uvc.c (USB video class, xHCI controllers only).
 */
#ifndef CAMERA_H
#define CAMERA_H
#include "types.h"

struct camera {
    const char *name;
    int w, h;
    int (*grab)(struct camera *c, uint32_t *px);        /* 0 on success */
    void *priv;
};

int  camera_register(struct camera *c);
int  camera_unregister(struct camera *c);
int  camera_count(void);
struct camera *camera_get(int i);
void camera_init(void);                                 /* registers the built-in test pattern */
#endif
