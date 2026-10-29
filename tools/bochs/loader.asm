; loader.asm - a tiny stand-in for GRUB, used only for automated testing in
; the Bochs emulator when GRUB/QEMU are not available.
;
; It does what a Multiboot loader does:
;   1. loads kernel.bin (flat image of the kernel) to physical 1 MiB
;   2. builds a Multiboot information structure (memory map, command line)
;   3. enters 32-bit protected mode with EAX=0x2BADB002, EBX=info and jumps
;      to the kernel entry point.
;
; Build: nasm -f bin -DKSECT=<sectors> -DENTRY=<hex> loader.asm -o loader.bin
bits 16
org 0x7C00

MBI     equ 0x1000
CMDLINE equ 0x1100
LOADER  equ 0x1180
MMAP    equ 0x1200
BUF     equ 0x8200

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [drive], dl
    mov si, dap                 ; load stage 2 (sectors 1-2) after this one
    mov ah, 0x42
    int 0x13
    jc fail1
    jmp stage2
fail1:
    hlt
    jmp fail1

dap:    db 16, 0
dap_count: dw 2
        dw 0x7E00, 0            ; offset, segment
dap_lba: dq 1
drive:  db 0

times 510 - ($ - $$) db 0
dw 0xAA55

stage2:
    in al, 0x92                 ; enable the A20 line
    or al, 2
    out 0x92, al

    lgdt [gdtr]                 ; "unreal mode": give FS a 4 GiB limit
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp short $ + 2
    mov bx, 0x10
    mov fs, bx
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax
    jmp 0:real
real:
    xor ax, ax
    mov ds, ax
    mov es, ax
    sti

    ; ---- load the kernel in 16 KiB chunks ----
    mov dword [dest], 0x100000
    mov dword [lba], 3
    mov word [dap + 4], BUF
    mov word [left], KSECT
.chunk:
    mov ax, [left]
    test ax, ax
    jz .loaded
    cmp ax, 32
    jbe .n
    mov ax, 32
.n:
    mov [cnt], ax
    mov [dap_count], ax
    mov ax, [lba]
    mov [dap_lba], ax
    mov si, dap
    mov dl, [drive]
    mov ah, 0x42
    int 0x13
    jc disk_error

    movzx ecx, word [cnt]
    shl ecx, 7                  ; sectors * 128 dwords
    mov esi, BUF
    mov edi, [dest]
.copy:
    mov eax, [esi]
    mov [fs:edi], eax
    add esi, 4
    add edi, 4
    dec ecx
    jnz .copy

    mov [dest], edi
    movzx eax, word [cnt]
    add [lba], ax
    sub [left], ax
    jmp .chunk
.loaded:

    ; ---- Multiboot information ----
    mov di, MBI
    xor ax, ax
    mov cx, 64
    rep stosw                   ; clear 128 bytes

    mov si, cmd_text            ; command line
    mov di, CMDLINE
.c1: lodsb
    stosb
    test al, al
    jnz .c1
    mov si, loader_text
    mov di, LOADER
.c2: lodsb
    stosb
    test al, al
    jnz .c2

    ; memory map via BIOS int 15h/E820, in Multiboot format (size, base, len, type)
    mov di, MMAP
    xor ebx, ebx
    mov dword [mmap_len], 0
    mov dword [mem_upper], 0
.e820:
    mov dword [di], 20          ; size field
    lea ax, [di + 4]
    push di
    mov di, ax
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 20
    int 0x15
    pop di
    jc .e820done
    cmp eax, 0x534D4150
    jne .e820done
    cmp dword [di + 4], 0x100000        ; usable region starting at 1 MiB?
    jne .skip
    cmp dword [di + 16], 1
    jne .skip
    mov eax, [di + 12]
    shr eax, 10
    mov [mem_upper], eax
.skip:
    add di, 24
    add dword [mmap_len], 24
    test ebx, ebx
    jnz .e820
.e820done:

    mov dword [MBI + 0],  0x245         ; flags: mem, cmdline, mmap, loader name
    mov dword [MBI + 4],  639
    mov eax, [mem_upper]
    mov [MBI + 8], eax
    mov dword [MBI + 16], CMDLINE
    mov eax, [mmap_len]
    mov [MBI + 44], eax
    mov dword [MBI + 48], MMAP
    mov dword [MBI + 64], LOADER

%ifdef VBE_MODE
    ; ---- graphics mode (what GRUB does when the kernel asks for a framebuffer) ----
    mov ax, 0x4F01              ; get mode information
    mov cx, VBE_MODE
    mov di, 0x1400
    int 0x10
    cmp ax, 0x004F
    jne .novbe
    mov ax, 0x4F02              ; set the mode with a linear framebuffer
    mov bx, VBE_MODE | 0x4000
    int 0x10
    cmp ax, 0x004F
    jne .novbe
    mov eax, [0x1400 + 0x28]    ; physical address of the framebuffer
    mov [MBI + 88], eax
    movzx eax, word [0x1400 + 0x10]
    mov [MBI + 96], eax         ; pitch
    movzx eax, word [0x1400 + 0x12]
    mov [MBI + 100], eax        ; width
    movzx eax, word [0x1400 + 0x14]
    mov [MBI + 104], eax        ; height
    mov al, [0x1400 + 0x19]
    mov [MBI + 108], al         ; bits per pixel
    mov byte [MBI + 109], 1     ; type 1 = RGB
    mov al, [0x1400 + 0x20]
    mov [MBI + 110], al         ; red position
    mov al, [0x1400 + 0x1F]
    mov [MBI + 111], al         ; red size
    mov al, [0x1400 + 0x22]
    mov [MBI + 112], al
    mov al, [0x1400 + 0x21]
    mov [MBI + 113], al
    mov al, [0x1400 + 0x24]
    mov [MBI + 114], al
    mov al, [0x1400 + 0x23]
    mov [MBI + 115], al
    or dword [MBI], 0x1000      ; flags bit 12: framebuffer info present
.novbe:
%endif

    ; ---- enter protected mode and jump to the kernel ----
    cli
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:pm
bits 32
pm:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7000
    mov eax, 0x2BADB002
    mov ebx, MBI
    mov ecx, ENTRY
    jmp ecx

bits 16
disk_error:
    mov si, err_text
.p: lodsb
    test al, al
    jz .h
    mov ah, 0x0E
    int 0x10
    jmp .p
.h: hlt
    jmp .h

align 4
gdt:
    dq 0
    dq 0x00CF9A000000FFFF       ; 0x08: 32-bit code, 4 GiB
    dq 0x00CF92000000FFFF       ; 0x10: 32-bit data, 4 GiB
gdtr:
    dw gdtr - gdt - 1
    dd gdt

cnt:    dw 0
left:   dw 0
lba:    dd 0
dest:   dd 0
mmap_len: dd 0
mem_upper: dd 0
cmd_text:    db CMDLINE_TEXT, 0
loader_text: db "MyOS test loader", 0
err_text:    db "Disk read error", 0

times 1536 - ($ - $$) db 0
