/*
 * boot.S - first code that runs in MyOS.
 *
 * GRUB (a Multiboot bootloader) loads the kernel, switches the CPU to
 * 32-bit mode and jumps to _start. This file:
 *   1. Declares the Multiboot header so GRUB recognizes the kernel.
 *   2. Reserves a stack (C code cannot run without one).
 *   3. Passes GRUB's information to kernel_main() and starts the kernel.
 */

/* ---------- Multiboot header ---------- */

.section .multiboot
.align 4

.set MB_MAGIC,    0x1BADB002                 /* "I am a Multiboot kernel"   */
.set MB_FLAGS,    0x00000003                 /* bit 0: align modules        */
                                             /* bit 1: give memory info     */
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)     /* magic+flags+checksum == 0   */

.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

/* ---------- Kernel stack (16 KiB) ---------- */

.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:                                   /* stacks grow downward        */

/* ---------- Entry point ---------- */

.section .text
.global _start
.type _start, @function

_start:
    cli                         /* disable interrupts                       */
    mov $stack_top, %esp        /* point the stack pointer at our stack     */
    xor %ebp, %ebp              /* zero frame pointer: bottom of the stack  */

    /*
     * GRUB leaves two values for us:
     *   EAX = 0x2BADB002 (magic number proving a Multiboot boot)
     *   EBX = address of the Multiboot info structure (memory size, etc.)
     * Pass them to kernel_main(magic, info) using the C calling convention:
     * arguments are pushed right to left, so push EBX first, then EAX.
     */
    sub $8, %esp                /* padding keeps the stack 16-byte aligned  */
    push %ebx                   /* 2nd argument: info pointer               */
    push %eax                   /* 1st argument: magic number               */

    call kernel_main            /* run the C kernel                         */

    /* kernel_main should never return. If it does, stop the CPU forever.  */
halt:
    cli
    hlt
    jmp halt

.size _start, . - _start

/* Tell the linker this file does not need an executable stack. */
.section .note.GNU-stack,"",@progbits