# MyOS

A minimal 32-bit x86 hobby operating system written from scratch in C and assembly. It boots with GRUB (Multiboot), prints to the VGA text screen, reads the PS/2 keyboard, and drops you into an interactive shell.

[![Latest release](https://img.shields.io/github/v/release/arshc0der/myOS)](https://github.com/arshc0der/myOS/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

```
========================
       Welcome to MyOS
========================
Type 'help' to list commands.

MyOS> help
MyOS shell commands
-------------------
help       - list commands
about      - about MyOS
version    - kernel version
clear      - clear screen
echo TEXT  - print text
...
```

## Features

- Multiboot-compliant boot (GRUB loads `kernel.elf`)
- Freestanding kernel with no libc
- VGA text-mode console (80x25) with scrolling, backspace and tab handling
- Polling PS/2 keyboard driver (scancode set 1) with Shift support
- Interactive shell with line editing
- Kernel panic handler (`kernel_panic`) that shows an error screen and halts the CPU

## Download

Ready-to-boot ISO images and source archives are on the [Releases page](https://github.com/arshc0der/myOS/releases). Each release includes SHA-256 checksums.

```bash
qemu-system-i386 -cdrom myos-1.1.0.iso -m 128M
```

## Shell commands

| Command | Description |
|---------|-------------|
| `help` | Display available commands |
| `about` | Display information about MyOS |
| `version` | Show the kernel version |
| `clear` | Clear the screen |
| `echo <text>` | Print text to the screen |
| `meminfo` | Display memory subsystem status |
| `cpuinfo` | Display architecture and kernel information |
| `date` | Display clock status |
| `whoami` | Show the current environment |
| `history` | Display command-history status |
| `halt` / `shutdown` | Halt the CPU |
| `reboot` | Not implemented yet |

Some commands (`meminfo`, `date`, `history`, `reboot`) are placeholders that report their unfinished features honestly.

## Project layout

```
myOS/
├── src/
│   ├── boot.S        # Multiboot header, stack setup, entry point (_start)
│   ├── kernel.c      # kernel_main: init console, start shell
│   ├── console.c     # VGA text console (clear, putc, write, scroll)
│   ├── keyboard.c    # PS/2 keyboard driver (getchar)
│   ├── shell.c       # Command-line shell
│   └── panic.c       # Kernel panic handler
├── iso/
│   └── boot/grub/grub.cfg   # GRUB menu entry
├── linker.ld         # Linker script (kernel loads at 1 MiB)
├── Makefile
└── LICENSE
```

## Requirements

- GCC with 32-bit support, and GNU Make
- GRUB tools (`grub2-mkrescue`, or `grub-mkrescue` on Debian/Ubuntu) and `xorriso`
- QEMU (`qemu-system-i386`) to run it; VirtualBox or VMware also work

**Fedora**

```bash
sudo dnf install gcc glibc-devel.i686 make qemu-system-x86 grub2-tools grub2-tools-extra xorriso
```

**Debian / Ubuntu**

```bash
sudo apt install build-essential gcc-multilib grub-pc-bin xorriso mtools qemu-system-x86
```

## Build

```bash
make clean
make                  # builds kernel.elf
```

Check that GRUB recognizes the kernel as Multiboot (optional):

```bash
grub2-file --is-x86-multiboot kernel.elf && echo "Multiboot OK"
```

If `grub2-file` is unavailable, use `grub-file` instead.

Build the bootable ISO:

```bash
cp kernel.elf iso/boot/kernel.elf
grub2-mkrescue -o myos.iso iso      # or: grub-mkrescue -o myos.iso iso
```

## Run

```bash
qemu-system-i386 -cdrom myos.iso -m 128M
```

A QEMU window opens with the MyOS welcome banner and shell prompt. Try:

```text
help
about
echo Hello from MyOS!
```

To boot the kernel directly, without an ISO:

```bash
qemu-system-i386 -kernel kernel.elf -m 128M
```

**VirtualBox / VMware:** create a 32-bit *Other/Unknown* VM with at least 64 MB of RAM and no hard disk, then attach `myos.iso` as the optical drive.

**Real hardware:** write the ISO to a USB drive or CD and boot in BIOS / legacy mode. Take care to select the correct device, as writing an image can erase existing data.

## How it works

1. GRUB reads `grub.cfg`, loads `kernel.elf` at 1 MiB and jumps to `_start` using the Multiboot protocol.
2. `boot.S` sets up a 16 KiB stack and calls `kernel_main`.
3. `kernel_main` clears the screen and starts the shell.
4. The shell prints a prompt, reads keys through the keyboard driver (polling ports `0x64` and `0x60`), and runs the command entered.

## Roadmap

- Interrupts (IDT, PIC) and an IRQ-driven keyboard
- Bootloader memory-map parsing and a physical page allocator
- Kernel heap (`malloc`)
- Real-time clock support for `date`
- Working `reboot`
- Command history
- Calling `kernel_panic` from kernel error paths
- A simple filesystem

## Contributing

Issues and pull requests are welcome. For larger changes, please open an issue first to discuss what you would like to change.

## License

Released under the [MIT License](LICENSE).
