# Architecture

Boot: GRUB loads kernel.elf (Multiboot header in src/boot/boot.S, requests a 1024x768x32
framebuffer) -> kernel_main (src/kernel/kernel.c).

Order: serial, console, GDT/IDT/PIC, PMM, VMM (paging), heap, graphics, scheduler, timer,
input, VFS, disks, then the `init` task runs selftest (optional), user_init(), the `gui`
option, and finally the shell.

Folders (include/ holds matching headers):

- src/mm: pmm.c (page allocator), vmm.c (paging), heap.c (kmalloc/kfree)
- src/proc: sched.c (tasks, policies), sync.c (mutex, semaphore, wait queue)
- src/fs: vfs.c, ramfs.c, devfs.c, myfs.c, blockdev.c, ramdisk.c, partition.c
- src/drivers: serial, vga, pit, keyboard, mouse, ata, pci, rtc, cpu
- src/gfx: framebuffer, drawing, fonts, text grid, framebuffer console
- src/gui: desktop and apps
- src/shell: command line and commands
- src/user: your code
- src/test: self tests

Adding a shell command: `SHELL_COMMAND(...)` places it in a linker section; no registration call.
Adding a filesystem: `REGISTER_FS(driver)` does the same for `.fs_drivers`.
