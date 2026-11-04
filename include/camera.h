/*
 * camera.h - video sources for the Camera app.
 *
 * A camera driver fills a struct camera and registers it. grab() writes one frame
 * of w*h pixels (0x00RRGGBB) into `px`. MyOS ships one built-in source, an animated
 * test pattern, so the Camera app works in any emulator. A USB webcam (UVC) driver
 * would register itself here too - it needs isochronous USB transfers, which the USB
 * stack does not have yet.
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
int  camera_count(void);
struct camera *camera_get(int i);
void camera_init(void);                                 /* registers the built-in test pattern */
#endif
