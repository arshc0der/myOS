# USB support (version 2.1.2)

Files: `src/drivers/usb/xhci.c` (controller + device setup), `src/drivers/usb/hid.c`
(keyboard, mouse and tablet reports), `include/usb.h`.

What works
- xHCI (USB 3) host controllers: modern PCs and laptops, QEMU/UTM `qemu-xhci`, VirtualBox, VMware.
- USB keyboards (boot protocol, key repeat, Caps Lock, Ctrl/Alt/Shift, F-keys, arrows).
- USB mice and touchpads that present themselves as a mouse (boot protocol).
- USB tablets (the pointer VMs give you): absolute position, parsed from the HID report descriptor.
- Plug and unplug while running (ports are rescanned about once a second).

Shell: `lsusb` lists the controller and devices. Boot option `nousb` disables the driver.

Not supported (yet)
- USB hubs (devices must be plugged straight into the computer's ports).
- Older controllers: EHCI, OHCI, UHCI.
- Touchpads that only speak the complex "precision touchpad" protocol or I2C-HID
  (many modern laptop built-in touchpads). These need a different driver.
- Mass storage, wheel scrolling, keyboard LEDs.

How it works in short: the controller and the kernel share rings of 16-byte entries (TRBs) in RAM.
The `usb` kernel task polls the event ring (no interrupts), and for each plugged device it
resets the port, assigns an address, reads the descriptors, selects the HID interfaces
and keeps one "interrupt IN" transfer queued per endpoint. Each completed transfer is a
report, which `hid.c` turns into the same `input_event`s the PS/2 drivers push.

Tested: enumeration in the Bochs emulator with an xHCI controller (keyboard, mouse, tablet) and
report decoding in `tests/host` (`make -C tests/host`). Not yet tested on real hardware.
