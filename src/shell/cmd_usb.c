/* cmd_usb.c - lsusb */
#include "shell.h"
#include "usb.h"

static int cmd_lsusb(int argc, char **argv)
{
    (void)argc; (void)argv;
    usb_dump();
    return 0;
}
SHELL_COMMAND("lsusb", "lsusb", "List USB devices", cmd_lsusb);
