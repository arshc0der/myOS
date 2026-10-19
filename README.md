# MyOS

A minimal 32-bit x86 hobby operating system written from scratch in C and assembly. It boots with GRUB (Multiboot), prints to the VGA text screen, reads the PS/2 keyboard, and drops you into an interactive shell.

[![Latest release](https://img.shields.io/github/v/release/arshc0der/myOS)](https://github.com/arshc0der/myOS/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

```
========================================
             Welcome to MyOS
========================================
Type 'help' to list commands.
PgUp/PgDn scroll the screen, Up/Down recall commands.

MyOS> sysinfo
System summary
--------------
OS                : MyOS 1.2.0 (32-bit x86)
CPU               : QEMU Virtual CPU version 2.5+
Memory            : 130559 KiB (127 MiB) usable
Bootloader        : GRUB 2.12
PCI devices       : 5
Virtualization    : yes (TCGTCGTCGTCG)
```

## Features

- Multiboot-compliant boot (GRUB loads `kernel.elf`)
- Freestanding kernel with no libc
- VGA text-mode console (80x25) with colors, a working hardware cursor and 275 lines of scrollback
- PS/2 keyboard driver (scancode set 1): Shift, Caps Lock, arrow keys, Page Up/Down, Home/End
- Interactive shell with command history (Up/Down) and backspace editing across wrapped lines
- Hardware information: memory map from GRUB, CPU details via CPUID, PCI device list, RTC date and time
- Kernel panic handler (`kernel_panic`) that shows an error screen and halts the CPU

## Download

Ready-to-boot ISO images and source archives are on the [Releases page](https://github.com/arshc0der/myOS/releases). Each release includes SHA-256 checksums.

```bash
qemu-system-i386 -cdrom myos-1.2.0.iso -m 128M
```

## Shell commands

| Command | Description |
|---------|-------------|
| `help` | List available commands |
| `about` | About MyOS |
| `version` | Show the kernel version |
| `sysinfo` | System summary (OS, CPU, memory, bootloader, PCI devices) |
| `meminfo` | Memory size, kernel location and the full memory map |
| `cpuinfo` | CPU vendor, model, family/model/stepping, features, virtualization |
| `lspci` | List PCI devices (network, storage, graphics, ...) |
| `date` | Date and time from the RTC clock |
| `history` | Commands typed so far |
| `echo <text>` | Print text to the screen |
| `clear` | Clear the screen and scrollback |
| `reboot` | Restart the computer |
| `halt` / `shutdown` | Halt the CPU |

**Keys:** `Up`/`Down` recall previous commands, `PgUp`/`PgDn` scroll the screen, `Home`/`End` jump to the oldest/newest output.

## Project layout

```
myOS/
├── src/
│   ├── boot.S        # Multiboot header, stack setup, entry point (_start)
│   ├── kernel.c      # kernel_main: init console, start shell
│   ├── console.c     # VGA console: colors, cursor, scrollback
│   ├── keyboard.c    # PS/2 keyboard driver
│   ├── keys.h        # Special key codes (arrows, Page Up/Down, ...)
│   ├── shell.c       # Command-line shell
│   ├── multiboot.c   # Boot information from GRUB (memory map, bootloader)
│   ├── cpu.c         # CPU information (CPUID)
│   ├── pci.c         # PCI device scanning
│   ├── rtc.c         # Real-time clock (date and time)
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
make iso            # copies kernel.elf into iso/ and runs grub(2)-mkrescue
```

Or do it by hand:

```bash
cp kernel.elf iso/boot/kernel.elf
grub2-mkrescue -o myos.iso iso      # or: grub-mkrescue -o myos.iso iso
```

## Run

```bash
make run            # builds the ISO and starts QEMU
# or, by hand:
qemu-system-i386 -cdrom myos.iso -m 128M
```

A QEMU window opens with the MyOS welcome banner and shell prompt. Try:

```text
sysinfo
meminfo
cpuinfo
lspci
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
- Physical page allocator (the memory map is already parsed)
- Kernel heap (`malloc`)
- Calling `kernel_panic` from kernel error paths
- Left/Right arrow editing inside the command line
- A simple filesystem

## Planned architecture

The roadmap below will move the kernel from a flat `src/` folder to a modular layout. The files under `arch/`, `drivers/`, `mm/`, `fs/` and `proc/` do not exist yet. This is the target structure, not the current one.

```
myOS/
├── boot/
├── src/
│   ├── kernel.c
│   ├── console.c
│   ├── keyboard.c
│   ├── shell.c
│   ├── arch/i386/
│   │   ├── gdt.c          # Global Descriptor Table
│   │   ├── idt.c          # Interrupt Descriptor Table
│   │   └── interrupts.S   # Interrupt/IRQ entry stubs
│   ├── drivers/
│   │   ├── timer.c        # PIT timer
│   │   └── disk.c         # Disk driver
│   ├── mm/
│   │   ├── physical.c     # Physical page allocator
│   │   └── heap.c         # Kernel heap
│   ├── fs/                # Filesystem
│   └── proc/              # Processes and scheduling
├── linker.ld
├── Makefile
└── iso/boot/grub/grub.cfg
```

Create the directories with:

```bash
mkdir -p src/arch/i386 src/drivers src/mm src/fs src/proc src/lib iso/boot/grub
```

## Contributing

Issues and pull requests are welcome. For larger changes, please open an issue first to discuss what you would like to change.

## License

Released under the [MIT License](LICENSE).
