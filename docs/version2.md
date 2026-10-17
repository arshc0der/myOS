mkdir -p src/arch/i386
mkdir -p src/drivers
mkdir -p src/mm
mkdir -p src/fs
mkdir -p src/proc
mkdir -p src/lib
mkdir -p iso/boot/grub

myOS/
├── boot/
├── src/
│   ├── kernel.c
│   ├── console.c
│   ├── keyboard.c
│   ├── shell.c
│   ├── arch/i386/
│   │   ├── gdt.c
│   │   ├── idt.c
│   │   └── interrupts.S
│   ├── drivers/
│   │   ├── timer.c
│   │   └── disk.c
│   ├── mm/
│   │   ├── physical.c
│   │   └── heap.c
│   ├── fs/
│   └── proc/
├── linker.ld
├── Makefile
└── iso/boot/grub/grub.cfg