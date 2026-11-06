# MyOS 2.13.0 — Horizon

**A hobby operating system built from scratch in C and x86 assembly.**

MyOS is an independent 32-bit x86 hobby operating system developed to explore low-level system programming, kernel architecture, memory management, process scheduling, filesystems, hardware drivers, networking, graphics, and graphical user interfaces.

Built from the ground up, MyOS boots through GRUB using the Multiboot protocol and is organized into modular kernel subsystems. What began as a small kernel with a text console has evolved into an operating system project with a graphical desktop, user accounts, persistent storage, system settings, device support, and a growing collection of desktop applications.

The **Horizon series** focuses on improving the desktop experience, expanding hardware compatibility, and introducing new operating system capabilities.

---

## Project Information

![Latest Release](https://img.shields.io/github/v/release/arshc0der/myOS?label=Latest%20Release&style=for-the-badge)
![License: MIT](https://img.shields.io/github/license/arshc0der/myOS?style=for-the-badge)
![GitHub Stars](https://img.shields.io/github/stars/arshc0der/myOS?style=for-the-badge&logo=github)
![GitHub Forks](https://img.shields.io/github/forks/arshc0der/myOS?style=for-the-badge&logo=github)
![GitHub Issues](https://img.shields.io/github/issues/arshc0der/myOS?style=for-the-badge&logo=github)
![Open Pull Requests](https://img.shields.io/github/issues-pr/arshc0der/myOS?style=for-the-badge&logo=github)
![Last Commit](https://img.shields.io/github/last-commit/arshc0der/myOS?style=for-the-badge&logo=git)
![Top Language](https://img.shields.io/github/languages/top/arshc0der/myOS?style=for-the-badge&logo=c)

| Property | Details |
|---|---|
| Project | MyOS |
| Current release | 2.13.0 |
| Series | Horizon |
| Architecture | 32-bit x86 |
| Primary languages | C and x86 Assembly |
| Boot protocol | GRUB / Multiboot |
| Filesystem | MyFS, with VFS support |
| License | MIT |
| Repository | [github.com/arshc0der/myOS](https://github.com/arshc0der/myOS) |

---

## Table of Contents

- [Overview](#overview)
- [Project Goals](#project-goals)
- [Latest Release: MyOS 2.13.0](#latest-release-myos-2130)
- [Features and Subsystems](#features-and-subsystems)
- [Desktop Environment](#desktop-environment)
- [Applications](#applications)
- [Hardware and Device Support](#hardware-and-device-support)
- [Networking and Connectivity](#networking-and-connectivity)
- [Screenshots and Demonstration](#screenshots-and-demonstration)
- [System Requirements](#system-requirements)
- [Getting Started](#getting-started)
- [Building MyOS](#building-myos)
- [Running MyOS](#running-myos)
- [Using the Shell](#using-the-shell)
- [Filesystems and Storage](#filesystems-and-storage)
- [Kernel Architecture](#kernel-architecture)
- [Project Structure](#project-structure)
- [Development and Customization](#development-and-customization)
- [Documentation](#documentation)
- [Testing and Verification](#testing-and-verification)
- [Known Limitations](#known-limitations)
- [Troubleshooting](#troubleshooting)
- [Development Roadmap](#development-roadmap)
- [Contributing](#contributing)
- [License](#license)

---

## Overview

MyOS is designed as a learning-oriented operating system that brings together fundamental operating system concepts and practical desktop functionality.

Rather than relying on an existing operating system kernel, MyOS implements its own kernel components, device interfaces, storage abstractions, graphical subsystem, and system utilities.

The project provides an environment for experimenting with:

- Kernel initialization and hardware abstraction.
- CPU architecture and interrupt handling.
- Physical and virtual memory management.
- Process scheduling and synchronization.
- Filesystem implementation and persistent storage.
- USB and PCI device discovery.
- Graphics rendering and window management.
- Input devices and desktop interaction.
- Network protocols and connectivity.
- Application development and system configuration.

MyOS is an ongoing hobby project. Features are introduced progressively, and implementation completeness, stability, and hardware compatibility vary between subsystems and releases.

## Project Goals

The main goals behind MyOS are:

**1. Operating System Development**

Explore how an operating system works internally, from its boot process and kernel initialization to memory management, device drivers, and system services.

**2. Modular Architecture**

Keep kernel components organized into independent subsystems with defined interfaces, making the codebase easier to understand, extend, and maintain.

**3. Graphical Desktop Experience**

Develop a usable desktop environment with windows, applications, system settings, notifications, and common desktop interactions.

**4. Hardware Compatibility**

Gradually expand support for storage devices, USB controllers, input devices, cameras, networking hardware, and other peripherals.

**5. Learning and Experimentation**

Provide an open environment for exploring systems programming, kernel engineering, graphics, filesystems, and hardware-level development.

---

## Latest Release: MyOS 2.13.0

<badge color="success">Latest documented release</badge>

MyOS 2.13.0 focuses on productivity improvements and expanded USB controller support.

### Notepad Improvements

Notepad now includes a dedicated menu bar with document and editing operations.

**File menu:**
- New
- Open
- Save
- Save As
- Close Window

**Edit menu:**
- Cut
- Copy
- Paste
- Delete Line
- Go to Line

Additional functionality includes:

- New documents use the `Untitled` name.
- Modified documents display `- Edited` in the window title.
- Save As prompts for a destination path.
- Keyboard shortcuts:
  - `Ctrl+S` — Save
  - `Ctrl+O` — Open
  - `Ctrl+N` — New

### Unsaved Changes Protection

Notepad now checks for unsaved modifications before closing a window, creating a new document, or opening another file.

A three-button dialog provides:

- **Save**
- **Don't Save**
- **Cancel**

The underlying GUI system also introduces:

- `gui_ask_save` — A reusable save-confirmation dialog.
- `close_request` — A hook that allows applications to handle window-close requests.

These interfaces provide a foundation for implementing unsaved-changes protection in other applications.

### Multi-Controller USB Support

USB controller handling has been expanded for computers containing multiple controllers.

Previously, MyOS used only the first detected xHCI controller and first detected EHCI controller. Devices connected to additional controllers could remain undetected.

The updated implementation supports up to:

- Three xHCI controllers.
- Three EHCI controllers.

For Intel 7-, 8-, and 9-series chipsets, USB ports are also switched from EHCI to xHCI.

**Testing status:** The release has been compile-checked but has not been run-tested.

For complete release information, see the [MyOS Releases](https://github.com/arshc0der/myOS/releases) page.

---

## Features and Subsystems

The following sections describe the major capabilities represented in the project documentation and development history. Not every feature has the same level of implementation or runtime verification.

### 1. Kernel and Hardware Foundation

- 32-bit x86 kernel written in C and assembly.
- GRUB boot support using the Multiboot protocol.
- Global Descriptor Table (GDT).
- Interrupt Descriptor Table (IDT).
- Interrupt service routines.
- Programmable Interrupt Controller (PIC).
- Programmable Interval Timer (PIT).
- CPU information through CPUID.
- PCI device enumeration.
- PS/2 keyboard and mouse support.
- Real-time clock (RTC).
- Serial and VGA console drivers.
- Kernel logging, formatted output, and panic handling.

### 2. Memory Management

MyOS contains separate memory-management components for handling physical memory, virtual memory, and dynamic kernel allocations.

Documented components include:

- Physical memory management using a bitmap allocator.
- Virtual memory management and paging.
- Kernel heap allocation.
- Memory-related diagnostic interfaces.

### 3. Processes and Synchronization

The process subsystem provides the foundation for scheduling and coordinating kernel execution.

Documented capabilities include:

- Scheduler subsystem with configurable policies.
- Context-switching support.
- Mutexes and semaphores.
- Synchronization primitives.
- Student scheduler implementation.

### 4. Filesystems and Storage

MyOS uses a Virtual Filesystem (VFS) abstraction to organize filesystem operations and storage devices.

Components include:

- Virtual filesystem.
- RAM filesystem (ramfs).
- Device filesystem (devfs).
- MyOS filesystem (MyFS).
- Block-device abstraction.
- RAM disks.
- ATA PIO disk driver.
- MBR partition support.
- Filesystem mounting and file operations through the shell.

### 5. Graphics and Desktop

The graphical subsystem provides the foundation for the MyOS desktop.

Capabilities include:

- Framebuffer graphics support.
- Framebuffer-based text console.
- Graphics primitives and terminal grid.
- Font rendering.
- Graphical user interface subsystem.
- Window management.
- Desktop applications.
- System settings and configuration.
- Screen magnification and visual accessibility features.

### 6. Networking and Connectivity

Networking development includes:

- Network stack development.
- Basic TCP client interfaces.
- HTTP GET functionality.
- Shell-based HTTP downloads through `wget`.
- Initial USB Bluetooth device detection and inquiry.
- PCI and USB network-device discovery.

### 7. Developer Tools

The repository also contains development infrastructure:

- Modular shell commands.
- Built-in kernel self-test sources.
- Host-side test infrastructure.
- Student templates for custom initialization.
- Filesystem development templates.
- Scheduler development templates.
- Architecture and subsystem documentation.

---

## Desktop Environment

The MyOS desktop has evolved beyond a basic graphical shell into a collection of integrated applications and system components.

### Window Management

- Graphical application windows.
- Window controls and maximization.
- Window switching.
- Desktop interaction and context menus.
- Close-request handling for applications.

### Desktop Interaction

The desktop context menu includes shortcuts for:

- Opening Terminal.
- Opening Files.
- Changing wallpaper.
- Opening Display Settings.
- Opening Control Center.
- Accessing Notifications and Calendar.
- Locking the screen.
- Sleep.
- Restart.
- Shut Down.

### Lock Screen

The lock screen provides:

- Dimmed wallpaper background.
- Large clock and date.
- Round user avatar.
- Password entry interface.

Waking from Sleep returns to the lock screen and requires the user's password.

### Wallpaper and Appearance

The Wallpaper page in Settings supports:

- 18 built-in colours and gradients.
- Custom pictures selected from `~/Pictures`.
- Custom image paths.
- Fill, Fit, Stretch, and Centre modes.
- Live wallpaper preview.
- Smooth cross-fade transitions.
- Persistent wallpaper preferences.

Wallpapers selected through Photos are also remembered after a restart.

### Dock

Dock visibility can be configured under **Settings → Dock**.

Available modes:

| Mode | Behaviour |
|---|---|
| Always show | Keeps the Dock visible |
| Hide when maximized | Hides the Dock when a window is maximized |
| Auto-hide | Hides the Dock and reveals it when the pointer reaches the bottom edge |

The Dock uses smooth slide animations, and maximized windows can use the space freed by the hidden Dock.

### Control Center and Notifications

The desktop includes a Control Center and Notification Center with calendar integration.

Documented functionality includes:

- Airplane Mode.
- Do Not Disturb.
- Brightness adjustment.
- Theme transitions.
- Notification banners.
- Calendar access.

### Accessibility

The project includes accessibility-oriented features such as:

- Screen magnification.
- Visual screen-reader captions.
- Identification of interface elements under the pointer.
- PC-speaker audio feedback.

The visual screen reader uses captions and tones rather than a full speech engine.

---

## Applications

MyOS includes a growing collection of graphical applications and system utilities.

| Application | Purpose |
|---|---|
| Files | Browse files, user folders, and available storage devices |
| Terminal | Execute shell commands and inspect the system |
| Notepad | Create, edit, open, and save text documents |
| Photos | View supported images and set pictures as wallpaper |
| Camera | Access supported USB and built-in cameras |
| Settings | Configure system preferences and hardware-related options |
| Task Manager | Inspect system activity |
| Assistant | Access typed commands for common system actions |
| System Information | View system and hardware information |

### Files

The Files application provides access to user folders and mounted storage.

Documented functionality includes:

- Home folders.
- Downloads, Documents, Pictures, Music, and Desktop folders.
- Disk and volume navigation.
- MyFS formatting for supported unformatted disks.
- Selected-item highlighting.
- Folder symbols and improved visual presentation.

### Terminal

Terminal provides an interactive command-line environment with system and filesystem utilities.

It also supports command history and text editing within the terminal interface.

### Notepad

Notepad supports document creation, editing, saving, and opening, with keyboard shortcuts and unsaved-changes protection.

### Photos

Photos provides image viewing with BMP, PNG, and JPEG decoding documented in the project history. It also supports setting an image as the desktop wallpaper.

### Camera

The Camera application uses the USB Video Class (UVC) driver for supported USB and built-in cameras.

Recent camera improvements include:

- Camera startup diagnostics.
- Alternative bandwidth and negotiation-size retries.
- Real-camera selection by default instead of the test pattern.
- Camera identification in `lsusb`.

The current driver has hardware compatibility limitations, particularly with older EHCI controllers and non-standard camera devices.

### Assistant

The Assistant application accepts typed requests for common system actions and information, including:

- Opening applications.
- Checking date and time.
- Changing light and dark appearance.
- Adjusting volume and mute settings.
- Controlling magnification and visual screen-reader features.
- Checking network and update status.
- Restarting and shutting down the system.

---

## Hardware and Device Support

Hardware compatibility is an ongoing development area.

### USB Controllers

MyOS includes USB controller support for xHCI and EHCI devices.

The 2.13.0 release expands controller handling to support up to three controllers of each type.

### Input Devices

Documented input support includes:

- PS/2 keyboard and mouse.
- USB mouse devices.
- PS/2 IntelliMouse wheel scrolling.
- USB mouse wheel scrolling.
- Supported USB and I2C-HID touchpad edge scrolling.

Scrolling is directed to the window beneath the pointer for Files, Terminal history, and Notepad. Other applications receive arrow-key input as their scrolling mechanism.

**Touchpad limitation:** PS/2 laptop touchpads currently behave as ordinary mice. Synaptics and ELAN gestures, including two-finger scrolling and pinch, require vendor protocols that are not implemented. Right-edge touchpad scrolling is currently supported on USB touchpads only.

### Camera Devices

The UVC camera driver supports USB and built-in camera devices connected through xHCI.

Current limitations include:

- EHCI camera streaming is not supported.
- Some non-standard laptop cameras may not be recognised.
- Camera privacy switches may prevent detection.

### Bluetooth

Initial USB Bluetooth support includes:

- Supported dongle registration.
- Device inquiry through the Scan action.
- Listing nearby Bluetooth devices and their transmitted names.

Pairing and Bluetooth audio are not implemented.

### Wi-Fi

Wi-Fi device discovery can identify supported PCI and USB wireless hardware, but a complete Wi-Fi driver is not available.

Per-chip firmware and WPA2 support are still required.

---

## Networking and Connectivity

Networking development is progressing through a modular network stack.

Documented interfaces include:

| Interface | Purpose |
|---|---|
| `net_tcp_connect` | Establish a TCP connection |
| `net_tcp_send` | Send TCP data |
| `net_tcp_recv` | Receive TCP data |
| `net_tcp_close` | Close a TCP connection |
| `net_http_get` | Perform an HTTP GET request |

The shell also includes a `wget` command for HTTP downloads.

These are basic networking capabilities rather than a complete modern networking implementation.

---

## Screenshots and Demonstration

Explore the MyOS graphical desktop and its applications.

<p align="center">
  <img src="docs/images/myos-showcase.gif" alt="MyOS Graphical Desktop Showcase" width="100%">
</p>

<p align="center">
  <strong>Explore the MyOS desktop experience.</strong><br>
  Desktop · Terminal · File Manager · Notepad · Task Manager · Settings
</p>

<details>
<summary>View individual desktop screenshots</summary>

### Desktop Environment

![MyOS Graphical Desktop](docs/images/desktop.png)

### System Information

![MyOS System Information](docs/images/activity.png)

### Terminal

![MyOS Terminal](docs/images/terminal.png)

### File Manager

![MyOS File Manager](docs/images/files.png)

### Notepad

![MyOS Notepad](docs/images/notepad.png)

### Task Manager

![MyOS Task Manager](docs/images/task-manager.png)

### System Settings

![MyOS Settings](docs/images/settings.png)

</details>

---

## System Requirements

A Linux development environment is recommended for building MyOS.

### Development Tools

| Tool | Purpose |
|---|---|
| GCC with 32-bit support | Compile kernel code |
| GNU Make | Build automation |
| GNU binutils | Assemble and link the kernel |
| GRUB utilities | Prepare a bootable ISO |
| xorriso | Create ISO images |
| QEMU | Run and debug MyOS |
| Bochs | Alternative emulation and debugging |

The exact toolchain requirements depend on the compiler flags and build targets configured in the Makefile.

### Fedora

```bash
sudo dnf install gcc glibc-devel.i686 make binutils \
  qemu-system-x86 grub2-tools grub2-tools-extra xorriso
```

### Debian / Ubuntu

```bash
sudo apt update

sudo apt install build-essential gcc-multilib binutils make \
  grub-pc-bin xorriso mtools qemu-system-x86
```

If package names differ on your distribution, install the equivalent packages.

---

## Getting Started

### 1. Clone the Repository

```bash
git clone https://github.com/arshc0der/myOS.git
cd myOS
```

### 2. Inspect the Build Targets

Check the available Makefile targets:

```bash
make help
```

If a help target is unavailable, inspect the Makefile:

```bash
grep -nE '^[[:alnum:]_.-]+:' Makefile
```

### 3. Build MyOS

Perform a clean build:

```bash
make clean
make
```

The expected kernel output is:

```text
kernel.elf
```

### 4. Build the Bootable ISO

If the ISO target is available:

```bash
make iso
```

The documented expected ISO output is:

```text
myos.iso
```

### 5. Run MyOS

Use the configured run target:

```bash
make run
```

Build target names and output locations should always be checked against the current Makefile.

---

## Running MyOS

### QEMU

QEMU allows you to experiment with MyOS without modifying your host operating system.

A basic launch command is:

```bash
qemu-system-i386 -cdrom myos.iso -m 128M
```

You can experiment with memory sizes and virtual hardware configurations as kernel support improves.

### VirtualBox and VMware

1. Create a virtual machine configured for a 32-bit x86 guest.
2. Attach the MyOS bootable ISO.
3. Configure the virtual hardware.
4. Boot from the virtual optical drive.

Compatibility depends on firmware mode and the devices exposed to the guest.

### Bochs

Bochs can be useful for low-level debugging and examining processor state.

The repository includes Bochs configuration and test tools under:

```text
tools/bochs/
```

Review the included configuration files for expected image paths and launch procedures.

### Real Hardware

Physical hardware compatibility depends on firmware, display initialization, storage controllers, USB controllers, and supported devices.

**Recommendation:** Test MyOS in a virtual machine first. Back up important data before attempting physical boot experiments.

---

## Using the Shell

After booting into the text-shell environment, use:

```text
help
```

This displays the commands registered in the running build.

### Core Commands

| Command | Description |
|---|---|
| `help` | List shell commands |
| `about` | Display project information |
| `version` | Display kernel version |
| `sysinfo` | Show a system summary |
| `meminfo` | Inspect memory information |
| `cpuinfo` | Display CPU details |
| `lspci` | List PCI devices |
| `date` | Display RTC date and time |
| `history` | Show command history |
| `echo <text>` | Print text |
| `clear` | Clear the console |
| `reboot` | Restart the system |
| `halt` | Halt the CPU |
| `shutdown` | Request system shutdown |

Additional filesystem, graphics, and process commands are implemented in separate shell modules:

```text
src/shell/cmd_fs.c
src/shell/cmd_gfx.c
src/shell/cmd_proc.c
src/shell/cmd_sys.c
```

Use the running shell's `help` output as the authoritative reference for command availability and syntax.

### Example Session

```text
MyOS> help
MyOS> version
MyOS> sysinfo
MyOS> cpuinfo
MyOS> meminfo
MyOS> lspci
```

Actual output depends on the emulator, available memory, virtual hardware, and current kernel build.

---

## Filesystems and Storage

MyOS separates filesystem operations from storage devices through a virtual filesystem and block-device layer.

### Filesystem Components

| Component | Source file | Purpose |
|---|---|---|
| Virtual filesystem | `src/fs/vfs.c` | Filesystem interface and namespace |
| RAM filesystem | `src/fs/ramfs.c` | RAM-backed files |
| Device filesystem | `src/fs/devfs.c` | Device namespace |
| MyFS | `src/fs/myfs.c` | Project-specific filesystem |
| Block devices | `src/fs/blockdev.c` | Block-device abstraction |
| RAM disk | `src/fs/ramdisk.c` | Memory-backed disk |
| Partition support | `src/fs/partition.c` | Partition handling |
| ATA driver | `src/drivers/ata.c` | ATA PIO storage access |

### Storage Features

- Virtual filesystem abstraction.
- RAM-backed storage.
- MyFS filesystem support.
- Device filesystem.
- ATA PIO disk access.
- MBR partition support.
- Filesystem mounting.
- User storage and mounted-volume management.

### Example Filesystem Workflow

The following is an illustrative workflow from the project's earlier documentation:

```text
MyOS> lsblk
MyOS> ramdisk create r1 2048
MyOS> mkfs myfs ram0
MyOS> mount ram0 /mnt myfs
MyOS> echo hello > /mnt/a.txt
MyOS> cat /mnt/a.txt
MyOS> df
```

Device names, mount syntax, formatting support, and shell redirection behaviour depend on the current implementation. Check `help` before using these commands.

For filesystem implementation details, see `docs/WRITING_A_FILESYSTEM.md`.

---

## Kernel Architecture

MyOS is organized into modular components to keep hardware-specific operations, memory management, filesystems, graphics, and process management separate.

### Memory Management

Memory-management code is located in:

```text
src/mm/
```

| File | Responsibility |
|---|---|
| `pmm.c` | Physical memory management |
| `vmm.c` | Virtual memory management |
| `heap.c` | Kernel heap allocation |

Related interfaces are declared in `include/`.

### Scheduling and Synchronization

The process subsystem is located in:

```text
src/proc/
```

| File | Responsibility |
|---|---|
| `sched.c` | Scheduling implementation |
| `sync.c` | Synchronization primitives |
| `src/arch/i386/switch.S` | Architecture-specific context switching |
| `src/user/my_scheduler.c` | Student scheduler implementation |

For additional details, see `docs/PROCESSES_AND_SCHEDULING.md`.

### Graphics and GUI

The graphics subsystem is organized into dedicated modules.

| File | Responsibility |
|---|---|
| `src/gfx/framebuffer.c` | Framebuffer handling |
| `src/gfx/fbcon.c` | Framebuffer console |
| `src/gfx/font.c` | Font rendering |
| `src/gfx/gfx.c` | Graphics operations |
| `src/gfx/tgrid.c` | Terminal grid |
| `src/gui/gui.c` | Graphical user interface |

Related interfaces are located in `include/`.

---

## Project Structure

MyOS uses a modular source layout to separate public interfaces, kernel implementations, architecture-specific code, documentation, and tests.

```text
myOS/
│
├── docs/
│   ├── ARCHITECTURE.md
│   ├── GUI_ROADMAP.md
│   ├── PROCESSES_AND_SCHEDULING.md
│   └── WRITING_A_FILESYSTEM.md
│
├── include/                 # Kernel headers and interfaces
│
├── iso/
│   └── boot/
│       └── grub/
│           └── grub.cfg     # GRUB boot menu
│
├── src/
│   ├── arch/i386/           # GDT, IDT, PIC, context switching
│   ├── boot/                # Assembly boot entry
│   ├── drivers/             # Hardware drivers
│   ├── fs/                  # VFS, MyFS, ramfs, devfs, storage
│   ├── gfx/                 # Framebuffer, fonts, graphics
│   ├── gui/                 # Graphical interface
│   ├── kernel/              # Kernel entry, console, panic, logging
│   ├── lib/                 # Kernel utility functions
│   ├── mm/                  # Physical and virtual memory
│   ├── proc/                # Scheduling and synchronization
│   ├── shell/               # Shell and command modules
│   ├── test/                # Kernel self-tests
│   └── user/                # Student extension implementations
│
├── templates/               # Student starter templates
│
├── tests/
│   └── host/                # Host-side test infrastructure
│
├── tools/
│   └── bochs/               # Bochs configuration and test tools
│
├── linker.ld                # Kernel linker script
├── Makefile                 # Build system
├── LICENSE
└── README.md
```

The structure above documents the project's major directories. Individual files and subdirectories may change as development continues.

---

## Development and Customization

MyOS includes starter files for experimenting with kernel functionality and implementing custom components.

### Student Extension Points

| File | Purpose |
|---|---|
| `src/user/user_init.c` | Custom initialization and user-defined shell commands |
| `src/user/studentfs.c` | Student filesystem implementation |
| `src/user/my_scheduler.c` | Student scheduler policy |
| `templates/user_init.c` | Initialization starter template |
| `templates/studentfs.c` | Filesystem development template |
| `templates/my_scheduler.c` | Scheduling starter template |

### Suggested Learning Path

1. Clone and build MyOS.
2. Boot it in QEMU.
3. Explore the system through the shell.
4. Read `docs/ARCHITECTURE.md`.
5. Experiment with `src/user/user_init.c`.
6. Study `src/fs/` before implementing filesystem functionality.
7. Read `docs/WRITING_A_FILESYSTEM.md`.
8. Explore `src/proc/` and the scheduler implementation.
9. Run available tests after making changes.

---

## Documentation

The `docs/` directory contains guides for the major project subsystems.

| Document | Description |
|---|---|
| `ARCHITECTURE.md` | Kernel architecture and subsystem design |
| `GUI_ROADMAP.md` | Graphical interface development plans |
| `PROCESSES_AND_SCHEDULING.md` | Process and scheduler design |
| `WRITING_A_FILESYSTEM.md` | Filesystem development guide |

Additional release-specific documentation is maintained alongside the corresponding release notes.

Documentation should be updated whenever the corresponding implementation or supported interfaces change.

---

## Testing and Verification

MyOS includes kernel self-test sources and host-side test infrastructure.

Relevant files include:

```text
src/test/selftest.c
src/test/selftest_fs.c
src/test/selftest_gfx.c
src/test/selftest_shell.c
tests/host/Makefile
```

### Kernel Tests

If the current kernel exposes a `selftest` command, run it from the MyOS shell:

```text
MyOS> selftest
```

### Host-Side Tests

Inspect the host test Makefile and run its available targets:

```bash
make -C tests/host
```

The existence of test source files does not guarantee that every test passes.

### MyOS 2.13.0 Verification Status

| Area | Status |
|---|---|
| Compilation | Compile-checked |
| Notepad runtime behaviour | Not runtime-tested |
| Unsaved-changes dialogs | Not runtime-tested |
| Multiple USB controllers | Not runtime-tested |
| Physical hardware compatibility | Not verified |

Runtime testing on emulators and physical hardware remains an important part of future development.

---

## Known Limitations

MyOS is an actively developed hobby operating system. Some subsystems are experimental or have limited hardware support.

### USB and Input Devices

- Multiple USB controller support has been expanded, but the latest implementation has not been runtime-tested.
- Some USB devices may require additional driver support.
- PS/2 laptop touchpads currently behave as ordinary mice.
- Synaptics and ELAN gestures, including two-finger scrolling and pinch, require vendor protocols that are not implemented.
- Touchpad right-edge scrolling is currently supported on USB touchpads only.

### Camera Support

- Camera streaming requires xHCI; EHCI camera streaming is not supported.
- Some non-standard laptop cameras may not be recognised by the UVC driver.
- Camera privacy switches can prevent camera detection.

### Networking

- Wi-Fi device discovery is available, but a complete Wi-Fi driver is not implemented.
- Wi-Fi requires chipset-specific firmware and WPA2 support.
- Basic TCP functionality does not yet provide a complete modern networking stack.
- TCP retransmission handling is not implemented.
- The documented HTTP functionality does not support HTTPS.

### Bluetooth

- Initial USB Bluetooth detection and scanning are available.
- Pairing, Bluetooth audio, and Bluetooth mouse support are not implemented.

### Accessibility and Audio

- The screen reader currently uses visual captions and PC-speaker tones.
- A full speech engine is not available.
- Microphone input is not supported.

### Updates and Localization

- Update detection is available, but automatic downloading and installation are not implemented.
- Kernel replacement remains a manual operation.
- Translation coverage is limited.
- Japanese and Chinese interfaces fall back to English because CJK font support is unavailable.

### Runtime Verification

The latest release, MyOS 2.13.0, has been compile-checked only. Runtime behaviour and compatibility must be verified separately.

---

## Troubleshooting

### GRUB Cannot Find or Load the Kernel

Check that the ISO contains the kernel at the path configured in:

```text
iso/boot/grub/grub.cfg
```

Rebuild the kernel and ISO:

```bash
make clean
make
make iso
```

### GRUB Utilities Are Missing

Install the GRUB utilities for your Linux distribution.

If available, check the kernel's Multiboot format:

```bash
grub-file --is-x86-multiboot kernel.elf
```

Some distributions provide `grub2-file` instead.

### The Desktop Does Not Appear

- Check the GRUB boot menu.
- Confirm that the graphical option is enabled in the current build.
- Try the text-shell option to distinguish a graphics issue from a general boot failure.

### ATA Disks Are Unavailable

Check whether the virtual machine exposes a compatible IDE/ATA device and whether the disk driver and block-device registration are initialized.

A RAM disk may be an alternative when supported by the current build.

### Keyboard or Mouse Input Does Not Work

Verify the virtual machine's input-device configuration and current driver support.

Device behaviour can differ between QEMU, Bochs, VirtualBox, VMware, and UEFI-based environments.

### USB Devices Are Not Detected

For USB controller or device detection issues, run:

```bash
lsusb
dmesg
```

Share the output, particularly log lines beginning with `usb`, along with your PC model and the affected device.

### Network or Bluetooth Features Do Not Work

Remember that hardware support varies by device and implementation. Check the available drivers and documented limitations before troubleshooting individual applications.

---

## Development Roadmap

Potential areas for future development include:

- Improving kernel robustness and error reporting.
- Expanding filesystem functionality and storage compatibility.
- Extending scheduler policies and synchronization primitives.
- Improving graphics, window management, and desktop usability.
- Adding Wi-Fi driver support.
- Expanding Bluetooth functionality, including pairing and audio.
- Improving TCP reliability and adding HTTPS support.
- Developing speech output and microphone support.
- Expanding automatic update functionality.
- Extending translation coverage and font support.
- Improving automated testing and emulator coverage.
- Expanding debugging tools and kernel diagnostics.
- Improving hardware compatibility across different x86 systems.

These are development directions, not promises of inclusion in a particular release.

---

## Contributing

Contributions, bug reports, documentation improvements, and new ideas are welcome.

### How to Contribute

1. Explore the existing source code and documentation.
2. Open an issue before making a large architectural change.
3. Keep changes focused on a specific subsystem.
4. Build and test your changes where possible.
5. Document new functionality and changes to existing interfaces.
6. Submit a pull request with a clear description of the changes.

### Bug Reports

When reporting an issue, include:

- MyOS version.
- Emulator or physical hardware used.
- Firmware mode and virtual hardware configuration.
- Build commands.
- Steps to reproduce the issue.
- Relevant logs and observed behaviour.

For hardware-related issues, include device identification information whenever possible.

**Repository:** [github.com/arshc0der/myOS](https://github.com/arshc0der/myOS)

---

## License

MyOS is released under the **MIT License**.

See the [LICENSE](LICENSE) file for the complete license terms.

---

<p align="center">
  <strong>MyOS 2.13.0 — Horizon</strong><br>
  Exploring operating system development, one subsystem at a time.
</p>