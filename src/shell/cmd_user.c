/* cmd_user.c - shell commands for accounts, disks, location and cameras. */
#include "shell.h"
#include "sys.h"
#include "blockdev.h"
#include "camera.h"
#include "image.h"
#include "vfs.h"
#include "kprintf.h"
#include "kstring.h"
#include "kerror.h"
#include "guiapi.h"

static int fail(const char *what, int rc)
{
    kprintf("%s: %s\n", what, kstrerror(rc));
    return 1;
}

static int cmd_users(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int i = 0; i < users_count(); i++)
        kprintf("  %-16s %s%s\n", users_get(i)->name, users_get(i)->admin ? "admin" : "user",
                users_current() && !strcmp(users_current()->name, users_get(i)->name) ? "   (logged in)" : "");
    if (!users_count())
        kprintf("  no accounts yet - the desktop (boot option \"gui\") asks for one on first start\n");
    kprintf("  /Users is %s\n", sys_users_persistent() ? "on a disk (accounts survive a restart)" : "in RAM (lost on restart)");
    return 0;
}
SHELL_COMMAND("users", "users", "List user accounts", cmd_users);

static int cmd_useradd(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: useradd <name> [password]\n"); return 1; }
    int rc = users_add(argv[1], argc > 2 ? argv[2] : "", users_count() == 0);
    return rc < 0 ? fail("useradd", rc) : 0;
}
SHELL_COMMAND("useradd", "useradd <name> [password]", "Create a user and its home folder /Users/<name>", cmd_useradd);

static int cmd_passwd(int argc, char **argv)
{
    if (argc < 3) { kprintf("usage: passwd <name> <new password>\n"); return 1; }
    int rc = users_set_password(argv[1], argv[2]);
    return rc < 0 ? fail("passwd", rc) : 0;
}
SHELL_COMMAND("passwd", "passwd <name> <password>", "Change a password", cmd_passwd);

static int cmd_userdel(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: userdel <name>\n"); return 1; }
    int rc = users_remove(argv[1]);
    return rc < 0 ? fail("userdel", rc) : 0;
}
SHELL_COMMAND("userdel", "userdel <name>", "Remove an account (the home folder stays)", cmd_userdel);

static int cmd_su(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: su <name> [password]\n"); return 1; }
    if (!users_check(argv[1], argc > 2 ? argv[2] : "")) { kprintf("su: wrong name or password\n"); return 1; }
    users_login(argv[1]);
    kprintf("now %s, in %s\n", argv[1], users_home());
    return 0;
}
SHELL_COMMAND("su", "su <name> [password]", "Switch user (changes to the home folder)", cmd_su);

static int cmd_location(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("  location: %s (UTC%+d:%02d)\n  available:", sys_prefs.city, sys_prefs.tz_minutes / 60,
                (sys_prefs.tz_minutes < 0 ? -sys_prefs.tz_minutes : sys_prefs.tz_minutes) % 60);
        for (int i = 0; i < sys_place_count(); i++) {
            const char *c; int tz;
            sys_place_at(i, &c, &tz);
            kprintf(" %s", c);
        }
        kprintf("\n  usage: location <city>   (use _ for spaces, e.g. New_York)\n");
        return 0;
    }
    for (int i = 0; i < sys_place_count(); i++) {
        const char *c; int tz;
        char cmp[32];
        sys_place_at(i, &c, &tz);
        strlcpy(cmp, c, sizeof cmp);
        for (char *p = cmp; *p; p++) if (*p == ' ') *p = '_';
        if (!strcmp(cmp, argv[1])) {
            sys_place_set(i);
            kprintf("location set to %s\n", c);
            return 0;
        }
    }
    kprintf("location: unknown city (run \"location\" for the list)\n");
    return 1;
}
SHELL_COMMAND("location", "location [city]", "Show or set the location / time zone", cmd_location);

static void show_dev(struct blockdev *d, void *ctx)
{
    (void)ctx;
    const char *mp = sys_volume_path(d->name);
    kprintf("  %-8s %-10s %6u MiB  %s\n", d->name, d->kind, d->sector_count / 2048, mp ? mp : "(not mounted)");
}

static int cmd_volumes(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "format")) {
        int rc = sys_volume_format(argv[2]);
        return rc < 0 ? fail("volumes format", rc) : 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "mount")) {
        int rc = sys_volume_mount(argv[2]);
        return rc < 0 ? fail("volumes mount", rc) : 0;
    }
    blockdev_for_each(show_dev, NULL);
    kprintf("  usage: volumes [mount <dev> | format <dev>]\n");
    return 0;
}
SHELL_COMMAND("volumes", "volumes [mount|format <dev>]", "List disks; mount one under /Volumes or format it as MyFS", cmd_volumes);

static int cmd_camera(int argc, char **argv)
{
    (void)argc; (void)argv;
    camera_init();
    for (int i = 0; i < camera_count(); i++)
        kprintf("  camera%d  %dx%d  %s\n", i, camera_get(i)->w, camera_get(i)->h, camera_get(i)->name);
    return 0;
}
SHELL_COMMAND("camera", "camera", "List video sources", cmd_camera);

static int cmd_imginfo(int argc, char **argv)
{
    const char *err;
    if (argc < 2) { kprintf("usage: imginfo <file.png|jpg|bmp>\n"); return 1; }
    struct gfx_surface *s = image_load(argv[1], GFX_WHITE, &err);
    if (!s) { kprintf("imginfo: %s\n", err); return 1; }
    kprintf("%s: decoded %dx%d\n", argv[1], s->w, s->h);
    gfx_surface_free(s);
    return 0;
}
SHELL_COMMAND("imginfo", "imginfo <file>", "Check that a picture can be decoded", cmd_imginfo);
