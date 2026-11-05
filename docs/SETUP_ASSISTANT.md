# Setup Assistant (first boot)

Source: `src/gui/setup.c`. Runs when `sys_prefs.setup_done` is false or no account exists.

Steps: Welcome, Hardware check, Language, Country/Region, Accessibility, Keyboard, Network,
Privacy, Account, Computer name, Date and time, Final preferences, Finishing, final Welcome.

- Back / Continue / Skip (optional steps), clickable progress bar to revisit steps, Esc = Back.
- Mouse and keyboard: Tab / arrows / Space / Enter.
- Input is validated (name, account name unique, password >= 4 and matching, valid computer name).
- Each step is saved to /Users/.sysconf as you go; finishing creates the account and sets setup_done.
- Boot option `nosetup` (or `guitest`) skips setup and creates a "demo" account.

All values live in `sys_prefs` (include/sys.h) and can be edited in Settings: Language, Accessibility,
Keyboard, Sound, Privacy, General (computer name, updates, voice, startup, Factory Reset), Hardware,
Date & Time, Network, Users, Storage, Devices, Display, Appearance.

Factory reset: Settings > General > "Reset settings" (keeps accounts) or "Erase everything" (accounts and
home folders too); shell: `factoryreset yes [erase]`. Both clear the setup flag; reboot runs the assistant.

Login screen (later boots): user list, password, Log In, Restart, Shut Down.

Limits: without a disk /Users is RAM and setup repeats; use the Storage step (or make run). Saved but not implemented yet:
screen reader, magnification, voice assistant, auto updates, language
translation, volume (no audio driver). Wi-Fi needs a driver (see NETWORK.md / wifi_register). Not tested at
run time: this release was only compiled.
