/*
 * boot.S - the very first code that runs.
 *
 * GRUB loads the kernel and jumps to _start with:
 *   eax = 0x2BADB002 (proof that a Multiboot loader started us)
 *   ebx = address of the Multiboot information structure
 * We set up a stack, clear the BSS, and call kernel_main(magic, info).
 */
    .set MB_MAGIC, 0x1BADB002
    /* flags: bit0 align modules, bit1 want memory map, bit2 want video mode.
       Asking for a linear framebuffer is what makes MyOS work on UEFI
       machines (UTM on Apple Macs, modern PCs), which have no VGA text mode. */
    .set MB_FLAGS, 0x00000007
    .set MB_CHECK, -(MB_MAGIC + MB_FLAGS)

    .section .multiboot, "a"
    .align 4
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHECK
    .long 0, 0, 0, 0, 0                  /* address fields (unused for ELF) */
    .long 0                              /* mode_type: 0 = linear graphics  */
    .long 1024                           /* preferred width                 */
    .long 768                            /* preferred height                */
    .long 32                             /* preferred bits per pixel        */

    .section .bss, "aw", @nobits
    .align 16
    .global boot_stack_bottom
boot_stack_bottom:
    .skip 16384
    .global boot_stack_top
boot_stack_top:

    .section .text, "ax"
    .global _start
    .type _start, @function
_start:
    cli
    movl %eax, %esi                 /* keep magic */
    movl $boot_stack_top, %esp

    movl $__bss_start, %edi         /* zero the BSS */
    movl $__bss_end, %ecx
    subl %edi, %ecx
    shrl $2, %ecx
    xorl %eax, %eax
    cld
    rep stosl

    subl $8, %esp                   /* keep the stack 16-byte aligned */
    pushl %ebx                      /* arg 2: multiboot info */
    pushl %esi                      /* arg 1: magic */
    call kernel_main
1:  cli
    hlt
    jmp 1b
