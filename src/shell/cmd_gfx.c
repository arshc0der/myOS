/* cmd_gfx.c - graphics related shell commands: gui, gfxinfo, splash */
#include "shell.h"
#include "gui.h"
#include "gfx.h"
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
