# System layout, users, disks and preferences

`include/sys.h` and `src/kernel/sys.c` set up the parts that make MyOS feel like an operating
system rather than a kernel demo.

## Folder layout

| Folder | Purpose |
|--------|---------|
| `/dev` | devices (`null`, `zero`, `console`, one file per disk: `hda`, `ram0` ...) |
| `/Volumes/<disk>` | every disk that holds a MyFS and could be mounted |
| `/Users/<name>` | home folder with Desktop, Documents, Downloads, Pictures, Music |
| `/System` | system data, shared icons in `/System/Icons/` |
| `/Applications` | reserved for your own programs |
| `/tmp`, `/etc`, `/mnt` | scratch, configuration, manual mounts |

## Disks

At boot, `sys_init()` looks at every block device that is not a RAM disk:

1. The first disk that contains a MyFS is mounted on **/Users**. Accounts, home folders and the
   system preferences are saved there, so they survive a restart.
2. Every other disk with a MyFS is mounted on `/Volumes/<name>`.
3. Disks without a MyFS stay unmounted. In **Files**, click the disk in the sidebar: MyOS offers to
   format it as MyFS. From the shell: `volumes format hdb`.

To make a persistent disk for the first time: run `volumes format hda` (or `hdb`), restart, and
create your account in the welcome screen. Without a MyFS disk, `/Users` lives in RAM and the
account is created again after every restart (the welcome screen says so).

Supported disk hardware: IDE/ATA (`hda`..`hdd`), RAM disks and GRUB modules. SATA in AHCI mode,
NVMe, virtio-blk and USB storage are **not** supported yet. In QEMU use `-drive file=disk.img,format=raw,if=ide`;
in UTM choose the IDE interface for the disk.

## Users

First start of the desktop with no account: the **Welcome** screen asks for a name and password
(the first account is an administrator). After that the **Log in** screen appears.

Shell commands: `users`, `useradd <name> [password]`, `passwd <name> <password>`, `userdel <name>`,
`su <name> [password]`. Settings > Users does the same with buttons.

Passwords are stored as a salted 32-bit hash. This keeps them out of sight; it is a teaching
example and not strong security.

## Preferences

`sys_prefs` (time zone, location, refresh rate, 12/24 hour clock) are saved to `/Users/.sysconf`
when `/Users` is on a disk. Settings > Date & Time lists 15 cities; the shell command
`location <city>` does the same. The hardware clock is read as UTC and the zone offset is added.

Settings > Display > Refresh rate (30, 60 or 100 Hz) sets how often the desktop redraws while
something moves; it is limited by the 100 Hz system timer. It does not change the refresh rate of
the host monitor.
