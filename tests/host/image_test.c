/* Host test for src/gfx/image.c: decodes the picture given as argv[1] and prints
 * "W H" followed by a few pixel values (compared with Pillow by tests/host/image_check.py). */
#include "image.h"
#include "vfs.h"
#include "heap.h"

int printf(const char *, ...);
void *malloc(unsigned long);
void free(void *);
void *fopen(const char *, const char *);
unsigned long fread(void *, unsigned long, unsigned long, void *);
int fclose(void *);

static const char *the_path;
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
void *kzalloc(size_t n) { void *p = malloc(n); if (p) { char *c = p; for (size_t i = 0; i < n; i++) c[i] = 0; } return p; }
struct gfx_surface *gfx_surface_create(int w, int h)
{
    struct gfx_surface *s = kzalloc(sizeof *s);
    s->px = kzalloc((size_t)w * h * 4);
    s->w = w; s->h = h; s->stride = w;
    return s;
}
void gfx_surface_free(struct gfx_surface *s) { free(s->px); free(s); }
int vfs_stat(const char *path, struct vfs_stat *st)
{
    void *f = fopen(path, "rb");
    if (!f) return -1;
    static unsigned char tmp[1 << 16];
    unsigned long total = 0, n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) total += n;
    fclose(f);
    st->type = VFS_FILE; st->size = (uint32_t)total;
    return 0;
}
ssize_t vfs_read_file(const char *path, void *buf, size_t len)
{
    void *f = fopen(path, "rb");
    ssize_t n = (ssize_t)fread(buf, 1, len, f);
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    const char *err = "";
    (void)argc;
    the_path = argv[1];
    struct gfx_surface *s = image_load(argv[1], 0xFFFFFF, &err);
    if (!s) { printf("ERR %s\n", err); return 1; }
    printf("%d %d\n", s->w, s->h);
    for (int i = 0; i < 6; i++) {
        int x = (s->w - 1) * i / 5, y = (s->h - 1) * i / 5;
        uint32_t c = s->px[y * s->stride + x];
        printf("%d %d %d %d %d\n", x, y, (c >> 16) & 255, (c >> 8) & 255, c & 255);
    }
    return 0;
}
