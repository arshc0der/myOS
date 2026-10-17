.section .multiboot
.align 4

.set MB_MAGIC,    0x1BADB002
.set MB_FLAGS,    0x00000003
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:

.section .text
.global _start
.type _start, @function

_start:
    cli
    mov $stack_top, %esp
    xor %ebp, %ebp

    call kernel_main

halt:
    cli
    hlt
    jmp halt

.size _start, . - _start

.section .note.GNU-stack,"",@progbits