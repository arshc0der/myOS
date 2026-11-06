# Control Center and Notification Center (v2.9.0)

Open the Control Center with the two-switch icon in the menu bar. Open the Notification Center by clicking the clock.
Click outside or press Esc to close. Panels slide and fade in 220 ms.

## Control Center
- Wi-Fi, Bluetooth, Airplane Mode, Ethernet, Do Not Disturb, Dark/Light mode tiles.
- Display slider: smooth brightness (eases to the new value). Sound slider with mute.
- Battery line and a battery icon in the menu bar on laptops. Shortcuts to Network and Sound settings.
- Changing the theme cross-fades from the old look.
- Settings are saved (needs the persistent disk, as for all settings).

## Notification Center
- Banners slide in from the top right (not shown during Do Not Disturb); click one to open the centre.
- List with per-item dismiss and Clear All. Do Not Disturb switch.
- Month calendar: previous/next month, today highlighted, dots on days with events.
  Keys: arrows select a day, PageUp/PageDown change month, N adds an event, Enter saves, Esc cancels.
- Events are saved to /Users/.calendar and shown as a notification on the day.

## Limits (honest)
- No Wi-Fi driver exists (needs firmware and an 802.11/WPA stack). The tile shows "No driver" until one registers.
- Bluetooth only discovers devices (USB adapters).
- Battery: presence/laptop detected from ACPI tables; the charge level needs an AML interpreter and is not shown.
- Brightness dims the picture in software; it does not change the backlight.
- Sound volume applies to the PC speaker only (no audio driver).
- Not tested at run time: only compiled.
