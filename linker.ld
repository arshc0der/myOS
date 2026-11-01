ENTRY(_start)

SECTIONS
{
    . = 1M;
    _kernel_start = .;

    .multiboot ALIGN(4) : { KEEP(*(.multiboot)) }
    .text   ALIGN(4096) : { *(.text .text.*) }
    .rodata ALIGN(4096) : { *(.rodata .rodata.*) }

    /* Shell commands register themselves in this table (see shell.h). */
    .shell_cmds ALIGN(4) : {
        __start_shell_cmds = .;
        KEEP(*(.shell_cmds))
        __stop_shell_cmds = .;
    }

    /* Filesystem drivers register themselves here (see vfs.h). */
    .fs_drivers ALIGN(4) : {
        __start_fs_drivers = .;
        KEEP(*(.fs_drivers))
        __stop_fs_drivers = .;
    }

    /* Desktop applications register themselves here (see guiapi.h). */
    .gui_apps ALIGN(4) : {
        __start_gui_apps = .;
        KEEP(*(.gui_apps))
        __stop_gui_apps = .;
    }

    .data   ALIGN(4096) : { *(.data .data.*) }
    .bss    ALIGN(4096) : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    . = ALIGN(4096);
    _kernel_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
