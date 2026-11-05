# MyOS build system.
#
#   make            build kernel.elf
#   make iso        build myos.iso (needs grub-mkrescue / grub2-mkrescue)
#   make run        run in QEMU (BIOS)
#   make run-uefi   run in QEMU with UEFI firmware (like UTM on a Mac)
#   make test       run the host unit tests (no emulator needed)
#   make clean
#
# Optional: CC=i686-elf-gcc (a cross compiler) on macOS:  make CC=i686-elf-gcc LD=i686-elf-ld

CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy
NASM    ?= nasm
BUILD   := build

CFLAGS  := -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-pic -fno-pie -m32 -Wall -Wextra -O2 -g \
           -fno-omit-frame-pointer -fno-asynchronous-unwind-tables \
           -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mno-avx \
           -Iinclude -MMD -MP
ASFLAGS := -m32 -Iinclude
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib -z max-page-size=0x1000 -z noexecstack
# Optional: link libgcc if you have a 32-bit one (we ship our own 64-bit
# division helpers, so this is normally empty).
LIBGCC  ?=

SRC_C   := $(shell find src -name '*.c' | sort)
SRC_S   := $(shell find src -name '*.S' | sort)
OBJS    := $(patsubst %.c,$(BUILD)/%.o,$(SRC_C)) $(patsubst %.S,$(BUILD)/%.o,$(SRC_S))

GRUB_MKRESCUE := $(shell command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null)

all: kernel.elf

# header dependencies (so changing a .h rebuilds the .c files that use it)
-include $(OBJS:.o=.d)

kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)
	@echo "Built $@ ($$(wc -c < $@) bytes)"

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

iso: kernel.elf
	@test -n "$(GRUB_MKRESCUE)" || { echo "grub-mkrescue not found (see docs/BUILDING.md)"; exit 1; }
	rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/grub
	cp kernel.elf $(BUILD)/iso/boot/kernel.elf
	cp iso/boot/grub/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o myos.iso $(BUILD)/iso
	@echo "Built myos.iso"

myos-disk.img:
	dd if=/dev/zero of=myos-disk.img bs=1M count=128 2>/dev/null
	@echo "Created myos-disk.img (blank 128 MB disk: MyOS keeps accounts and settings here)"

run: iso myos-disk.img
	qemu-system-i386 -cdrom myos.iso -hda myos-disk.img -boot d -m 128M -serial stdio -vga std -device qemu-xhci -device usb-tablet

run-uefi: iso myos-disk.img
	@echo "Needs OVMF firmware; see docs/BUILDING.md"
	qemu-system-x86_64 -cdrom myos.iso -hda myos-disk.img -m 256M -serial stdio -bios $(OVMF) -vga std -device qemu-xhci -device usb-tablet

test:
	$(MAKE) -C tests/host

clean:
	rm -rf $(BUILD) kernel.elf myos.iso

.PHONY: all iso run run-uefi test clean
