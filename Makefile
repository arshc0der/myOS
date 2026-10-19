CC = gcc
AS = gcc
LD = gcc

CFLAGS = -std=gnu11 -ffreestanding -fno-builtin \
         -fno-stack-protector -fno-pic -fno-pie \
         -m32 -Wall -Wextra -O2

ASFLAGS = -m32 -ffreestanding

LDFLAGS = -T linker.ld -ffreestanding -nostdlib \
          -no-pie -m32

SOURCES = src/kernel.c src/console.c src/keyboard.c \
          src/shell.c src/panic.c src/multiboot.c \
          src/cpu.c src/pci.c src/rtc.c

OBJECTS = $(SOURCES:.c=.o) src/boot.o

# GRUB's ISO tool is called grub2-mkrescue on Fedora, grub-mkrescue elsewhere.
GRUB_MKRESCUE := $(shell command -v grub2-mkrescue 2>/dev/null || \
                         command -v grub-mkrescue 2>/dev/null)

all: kernel.elf

kernel.elf: $(OBJECTS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) -lgcc

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

src/boot.o: src/boot.S
	$(AS) $(ASFLAGS) -c $< -o $@

# The shell and keyboard share key codes through keys.h.
src/shell.o src/keyboard.o: src/keys.h

# make iso : build the bootable ISO image
iso: myos.iso

myos.iso: kernel.elf iso/boot/grub/grub.cfg
	cp kernel.elf iso/boot/kernel.elf
	$(GRUB_MKRESCUE) -o $@ iso

# make run : build and start MyOS in QEMU
run: myos.iso
	qemu-system-i386 -cdrom myos.iso -m 128M

clean:
	rm -f $(OBJECTS) kernel.elf myos.iso iso/boot/kernel.elf

.PHONY: all iso run clean
