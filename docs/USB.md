# USB in MyOS (version 2.2.0)

## What is supported

| Part | Status |
|------|--------|
| xHCI (USB 3) controller | supported (first controller only) |
| EHCI (USB 2) controller | supported; low/full speed ports are handed to the companion UHCI/OHCI controller |
| OHCI and UHCI (USB 1.1) controllers | supported |
| USB hubs | supported (up to 8 ports per hub, 5 levels deep), hot plug is polled about once a second |
| Keyboards, mice, tablets | supported (HID boot protocol or report descriptor) |
| Precision touchpads (USB) | supported: finger motion moves the pointer, a quick tap clicks, the pad button clicks |
| I2C / I2C-HID touchpads (built into most laptops) | NOT supported (see below) |
| Mass storage, audio, etc. | not supported |

PS/2 keyboard and mouse still work at the same time as USB.

## How the code is organised

```
include/usbcore.h        interface between controller drivers and the shared layer
src/drivers/usb/usbcore.c  enumeration, hubs, HID setup, hot plug, the "usb" task, lsusb
src/drivers/usb/xhci.c   xHCI driver
src/drivers/usb/ehci.c   EHCI driver
src/drivers/usb/ohci.c   OHCI driver
src/drivers/usb/uhci.c   UHCI driver
src/drivers/usb/hid.c    HID reports -> keyboard / mouse events
```

A controller driver fills in `struct usb_hc_ops` (give a device an address, run a
control transfer, keep an interrupt-IN transfer running, reset a root port) and
calls `usb_hc_add()`. Everything else is shared, so adding another controller
type only needs one new file plus one line in `usb_task()`.

## Limits you should know about

- I2C touchpads: most laptop touchpads are not USB. They are connected over I2C
  and described by ACPI. That needs an ACPI interpreter and an I2C controller
  driver, which MyOS does not have. On such laptops the touchpad will not work
  yet; use PS/2 mode (many laptops offer it), a USB mouse, or the F2 keyboard
  pointer mode.
- EHCI cannot talk to a low/full speed device that is plugged into a
  high-speed hub (it needs "split transactions"). Plug such devices into a
  root port, or use a controller that supports them (xHCI, or the companion
  controller). A message is printed when this happens.
- Only the first xHCI controller is used.
- Real hardware: everything was tested in the Bochs emulator only (xHCI, EHCI,
  OHCI, UHCI, a hub with children, companion hand-over). Real computers may need
  small fixes, because every vendor's controller has quirks.

## Debugging

- `lsusb` lists controllers and devices (hub children are indented).
- Boot option `nousb` turns USB off. Boot option `usbdump` prints the list
  3 seconds after boot (used by the emulator tests).
- Host test for report parsing: `make -C tests/host`.
