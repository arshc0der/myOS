
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .text ALIGN(4K) :
    {
        KEEP(*(.multiboot))
        *(.text*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata*)
    }

    .data ALIGN(4K) :
    {
        *(.data*)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss*)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note.gnu.property)
        *(.note.gnu.build-id)
    }
}
