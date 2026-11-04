/* camera.c - registry of video sources plus the built-in test-pattern camera. */
#include "camera.h"
#include "timer.h"

#define MAX_CAMS 4
static struct camera *cams[MAX_CAMS];
static int ncams;

int camera_register(struct camera *c)
{
    if (ncams >= MAX_CAMS)
        return -1;
    cams[ncams++] = c;
    return 0;
}
int camera_count(void) { return ncams; }
struct camera *camera_get(int i) { return i >= 0 && i < ncams ? cams[i] : NULL; }

/* moving colour bars with a bouncing square */
static int pattern_grab(struct camera *c, uint32_t *px)
{
    static const uint32_t bars[7] = { 0xFFFFFF, 0xFFE600, 0x00E5FF, 0x00D84A, 0xFF2DCB, 0xFF3B30, 0x2F54EB };
    int t = (int)(timer_ticks() / 2);
    int sq = c->h / 5;
    int bx = (t * 3) % (2 * (c->w - sq));
    int by = (t * 2) % (2 * (c->h - sq));
    if (bx >= c->w - sq) bx = 2 * (c->w - sq) - bx;
    if (by >= c->h - sq) by = 2 * (c->h - sq) - by;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            uint32_t col = bars[(x * 7 / c->w + 0) % 7];
            if (y > c->h * 3 / 4)
                col = (uint32_t)((x * 255 / c->w) * 0x010101);
            if (x >= bx && x < bx + sq && y >= by && y < by + sq)
                col = 0x101010;
            px[y * c->w + x] = col;
        }
    return 0;
}

static struct camera test_cam = { .name = "Test pattern (virtual camera)", .w = 320, .h = 240, .grab = pattern_grab };

void camera_init(void)
{
    static bool done;
    if (!done) {
        done = true;
        camera_register(&test_cam);
    }
}
