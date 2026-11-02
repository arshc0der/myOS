/* cmd_gfx.c - graphics related shell commands: gui, gfxinfo, splash */
#include "shell.h"
#include "gui.h"
#include "gfx.h"
#include "fbcon.h"
#include "kprintf.h"

static int cmd_gui(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!gui_supported()) {
        kprintf("gui: no graphics framebuffer. Boot the \"graphical desktop\" entry\n"
                "     (on UEFI machines, this is the default).\n");
        return 1;
    }
    gui_run();
    return 0;
}
SHELL_COMMAND("gui", "gui", "Start the graphical desktop", cmd_gui);

static int cmd_gfxinfo(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!gfx_available()) {
        kprintf("No graphics framebuffer (text mode only).\n");
        return 1;
    }
    kprintf("Graphics: %dx%d\n", gfx_width(), gfx_height());
    return 0;
}
SHELL_COMMAND("gfxinfo", "gfxinfo", "Show the screen resolution", cmd_gfxinfo);

static int cmd_splash(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!gfx_available())
        return 1;
    gui_splash();
    return 0;
}
SHELL_COMMAND("splash", "splash", "Show the boot logo screen", cmd_splash);

static int cmd_resolution(int argc, char **argv)
{
    int n = gfx_mode_count();

    if (!gfx_available()) {
        kprintf("No graphics framebuffer.\n");
        return 1;
    }
    if (argc < 2) {
        kprintf("Current: %dx%d\n", gfx_width(), gfx_height());
        if (!n) {
            kprintf("This machine cannot change the resolution at run time. Use the boot menu\n"
                    "\"Display resolution\" instead.\n");
            return 0;
        }
        kprintf("Available:");
        for (int i = 0; i < n; i++) {
            int w, h;
            gfx_mode_at(i, &w, &h);
            kprintf(" %dx%d", w, h);
        }
        kprintf("\nUsage: resolution 1280x800\n");
        return 0;
    }
    int w = 0, h = 0;
    const char *p = argv[1];
    while (*p >= '0' && *p <= '9') w = w * 10 + (*p++ - '0');
    if (*p == 'x' || *p == 'X')
        p++;
    while (*p >= '0' && *p <= '9') h = h * 10 + (*p++ - '0');
    if (!w || !h) {
        kprintf("Usage: resolution WIDTHxHEIGHT\n");
        return 1;
    }
    if (!gfx_set_mode(w, h)) {
        kprintf("Cannot switch to %dx%d here (see `resolution` for the list).\n", w, h);
        return 1;
    }
    fbcon_resize();
    kprintf("Resolution is now %dx%d\n", gfx_width(), gfx_height());
    return 0;
}
SHELL_COMMAND("resolution", "resolution [WxH]", "Show or change the screen resolution (virtual machines)", cmd_resolution);
