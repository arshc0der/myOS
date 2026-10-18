# MyOS v1.0.0

A tiny 32-bit x86 operating system written from scratch in C and assembly. It boots with GRUB (Multiboot), prints to the VGA text screen, reads the PS/2 keyboard, and drops you into a small interactive shell.

```
Starting MyOS kernel...
Welcome to MyOS!
Type help to begin.

MyOS> help
Commands:
  help   - show commands
  about  - about MyOS
  clear  - clear screen
  echo   - print text
  halt   - halt CPU
```

## Features

- Multiboot-compliant boot (GRUB loads `kernel.elf`)
- Freestanding kernel, no libc
- VGA text-mode console (80x25) with scrolling, backspace and tab
- Polling PS/2 keyboard driver (scancode set 1) with Shift support
- Interactive shell with line editing

## Shell commands

| Command | Description |
|---------|-------------|
| `help`  | Show the list of commands |
| `about` | Show information about MyOS |
| `clear` | Clear the screen |
| `echo <text>` | Print text back to the screen |
| `halt`  | Stop the CPU |

## Project layout

```
myOS/
├── src/
│   ├── boot.S        # Multiboot header, stack setup, entry point (_start)
│   ├── kernel.c      # kernel_main: init console, start shell
│   ├── console.c     # VGA text console (clear, putc, write, scroll)
│   ├── keyboard.c    # PS/2 keyboard driver (getchar)
│   └── shell.c       # Command-line shell
├── iso/
│   └── boot/grub/grub.cfg   # GRUB menu entry
├── linker.ld         # Linker script (kernel loads at 1 MiB)
└── Makefile
```

## Requirements

- `gcc` with 32-bit support, and `make`
- GRUB tools to build the ISO (`grub2-mkrescue`, or `grub-mkrescue` on Debian/Ubuntu), plus `xorriso`
- `qemu-system-i386` to run it (a VM such as VirtualBox also works)

**Fedora:**

```bash
sudo dnf install gcc glibc-devel.i686 make qemu-system-x86 grub2-tools grub2-tools-extra xorriso
```

**Debian/Ubuntu:**

```bash
sudo apt install build-essential gcc-multilib grub-pc-bin xorriso mtools qemu-system-x86
```

## Build

```bash
make clean
make            # builds kernel.elf
```

### Check that GRUB recognizes the kernel as Multiboot

```bash
grub2-file --is-x86-multiboot kernel.elf && echo "Multiboot OK"
```

If `grub2-file` is unavailable, use `grub-file --is-x86-multiboot kernel.elf`. No output and exit code 0 means success.

### Build the ISO

```bash
cp kernel.elf iso/boot/kernel.elf
grub2-mkrescue -o myos.iso iso      # or: grub-mkrescue -o myos.iso iso
```

## Run

```bash
qemu-system-i386 -cdrom myos.iso -m 128M
```

A QEMU window opens with the MyOS welcome message and shell prompt. Try:

```text
help
about
echo Hello from MyOS!
```

Press Enter after each command.

To boot the kernel directly, without an ISO:

```bash
qemu-system-i386 -kernel kernel.elf -m 128M
```

In VirtualBox/VMware, create a 32-bit "Other" VM and attach `myos.iso` as the CD drive. It also boots on real hardware from USB or CD (BIOS/legacy boot).

## How it works

1. GRUB reads `grub.cfg`, loads `kernel.elf` at 1 MiB and jumps to `_start` using the Multiboot protocol.
2. `boot.S` sets up a 16 KiB stack and calls `kernel_main`.
3. `kernel_main` clears the screen and starts the shell.
4. The shell prints a prompt, reads keys through the keyboard driver (polling ports `0x64`/`0x60`), and runs the command entered.

## Roadmap ideas

- Interrupts (IDT, PIC) and an IRQ-driven keyboard
- Memory management (physical allocator, `malloc`)
- More shell commands (`reboot`, `time`, `mem`)
- A simple filesystem

## License

Released under the [MIT License](LICENSE).
