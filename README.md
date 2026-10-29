# MyOS 2.0.0 "Foundation"

A 32-bit x86 hobby operating system written in C and assembly, booted with GRUB (Multiboot).
Version 2 turns it into a platform you can write your own filesystems and schedulers on,
and adds a graphical desktop.

## Features

- Physical and virtual memory manager (bitmap allocator, paging, kernel heap)
- Preemptive multitasking with pluggable scheduler policies (round robin, priority, yours)
- Mutexes, semaphores and wait queues
- Virtual file system with mount points, file descriptors and a working directory per task
- Filesystems: ramfs (RAM), devfs (/dev), MyFS (on-disk, written for this project)
- Block devices: RAM disks, ATA hard disks (PIO), MBR partitions
- Keyboard and PS/2 mouse input
- Framebuffer graphics (works on UEFI/Mac), text console drawn on the framebuffer
- Graphical desktop: windows, taskbar, start menu, Terminal, Files, Notepad, System Info
- Shell with about 60 commands, redirection (`>`), history, scrollback
- 200 built-in self tests (`selftest`)

## Build and run

Requirements: gcc (with 32-bit support), binutils, make, grub-mkrescue + xorriso, qemu (optional).

    make            # builds kernel.elf
    make iso        # builds myos.iso
    make run        # run in QEMU

The boot menu offers: Graphical desktop, Text shell, Self test.

## Mac / UTM

Use a UEFI VM and boot the ISO. The old error "no suitable video mode found" is fixed:
the kernel asks GRUB for a 1024x768 framebuffer and draws its own text on it.
For hard disk (ATA) support choose an IDE drive; otherwise use RAM disks (`ramdisk`).
The mouse may not work in some VMs; the desktop is fully usable with the keyboard.

## Desktop keys

Alt+T Terminal, Alt+F Files, Alt+I System Info, Alt+N Notepad, Alt+A About,
Alt+Tab switch window, Alt+W close window, Alt+M start menu, Alt+Q quit to text shell.
No mouse (UTM USB tablet is not supported): Alt+arrow keys move the pointer (add Shift for bigger steps), Alt+Enter clicks.
Notepad: Ctrl+S saves. Files: Enter opens, Backspace goes up, D deletes.

## Try the filesystems

    lsblk                       # list disks
    ramdisk create r1 2048      # (see `help ramdisk`)
    mkfs myfs ram0
    mount ram0 /mnt myfs
    echo hello > /mnt/a.txt
    cat /mnt/a.txt
    df

## Write your own code

Everything for students is in `src/user/` (templates also in `templates/`):

| File | What |
|------|------|
| `user_init.c` | your boot code and custom shell commands (`hello`, `fsdemo` examples) |
| `studentfs.c` | skeleton filesystem `hellofs`; try `mount none /h hellofs` |
| `my_scheduler.c` | your scheduler policy; select with `sched mine` |

See `docs/` for guides.

## Honest status

Tested in the Bochs emulator: kernel, memory, scheduler, filesystems, shell, framebuffer console
(200/200 self tests). The graphical desktop, GRUB menu and the Mac/UEFI boot were written
but not run before release; please report bugs.

## License

MIT, see LICENSE.
