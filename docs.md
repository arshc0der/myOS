MyOS/
├── boot/
│   └── boot.asm
├── src/
│   ├── kernel.c
│   ├── console.c
│   ├── keyboard.c
│   └── shell.c
├── linker.ld
├── Makefile
└── grub.cfg


set timeout=0
set default=0

menuentry "MyOS" {
    multiboot /boot/kernel.elf
    boot
}

Check whether GRUB recognizes the kernel as Multiboot:

make clean 
make

bash
grub2-file --is-x86-multiboot kernel.elf
If that succeeds, copy the kernel and build the ISO:


bash
cp kernel.elf iso/boot/kernel.elf
grub2-mkrescue -o myos.iso iso
If grub2-file is unavailable, check grub-file --is-x86-multiboot kernel.elf instead.

Step 7: Boot MyOS in QEMU
Run:


bash
qemu-system-i386 -cdrom myos.iso -m 128M
A QEMU window should open. If everything is configured correctly, you should see your MyOS welcome message and shell prompt.

Try typing:


text
help
about
echo Hello from MyOS!
Press Enter after each command.

Step 1: Install the boot and testing tools
On Fedora, run:


bash
sudo dnf install qemu-system-x86 grub2-tools grub2-tools-extra xorriso make