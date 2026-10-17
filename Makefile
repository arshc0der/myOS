
CC = gcc
AS = gcc
LD = gcc

CFLAGS = -std=gnu11 -ffreestanding -fno-builtin \
         -fno-stack-protector -fno-pic -fno-pie \
         -m32 -Wall -Wextra -O2

ASFLAGS = -m32 -ffreestanding

LDFLAGS = -T linker.ld -ffreestanding -nostdlib \
          -no-pie -m32

SOURCES = src/kernel.c src/console.c \
          src/keyboard.c src/shell.c src/panic.c

OBJECTS = $(SOURCES:.c=.o) src/boot.o

all: kernel.elf

kernel.elf: $(OBJECTS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) -lgcc

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

src/boot.o: src/boot.S
	$(AS) $(ASFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) kernel.elf myos.iso

.PHONY: all clean
