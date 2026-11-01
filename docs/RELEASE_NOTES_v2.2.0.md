# MyOS 2.2.0 "Horizon"

## User interface
- New macOS-style look: simple white (light) and black (dark) themes, rounded windows with soft shadows, a menu bar, a dock with zoom and running dots, and Spotlight-style search (Alt+Space).
- Window buttons are monochrome by default (colour is optional in Settings).

## Code structure (for adding your own programs)
- The GUI core (windows, dock, menu bar, input) is in `src/gui/gui.c`.
- Every app is its own file in `src/gui/apps/` (Terminal, Files, Notepad, Activity, Settings, About). Register an app with `GUI_APP(...)`; see `src/user/hello_app.c` for a small example. No change to the core is needed.
- Shared drawing helpers and the themes are in `src/gui/widgets.c`; the public interface is `include/guiapi.h`.

## USB
- New shared USB layer (`usbcore.c`) with support for hubs.
- New drivers: EHCI, OHCI, UHCI (xHCI was converted to the shared layer).
- Precision touchpads over USB.
- Not supported: I2C touchpads (needs ACPI and I2C drivers), low/full speed devices behind high-speed hubs on EHCI.
- Tested in the Bochs emulator only; real hardware may need fixes. PS/2 still works alongside USB.

See docs/USB.md.

## 2.2.1 fixes
- Yellow screen fix: the display colour layout reported by the boot loader is now checked; if it is missing or wrong, standard RGB is used. Settings > Display > "Colour order" (Auto / RGB / BGR) fixes any remaining wrong colours at run time, and the boot option `fbfmt=bgr` (or `fbfmt=rgb`) does the same.
- Accent colours: Blue (default), Purple, Pink, Red, Orange, Green, Graphite. Settings > Appearance has Theme (Light/Dark), Style (Colorful/Monochrome), Accent colour and Window buttons.
