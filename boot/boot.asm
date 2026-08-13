BITS 32

; Multiboot2 header for GRUB (i386)
section .multiboot_header
align 8
header_start:
    dd 0xe85250d6                ; Magic (multiboot2)
    dd 0                         ; Architecture i386
    dd header_end - header_start
    dd -(0xe85250d6 + 0 + (header_end - header_start))

    ; Framebuffer tag: prefer 1024x768x32
    align 8
    dw 5                         ; type = framebuffer
    dw 0                         ; flags
    dd 20                        ; size
    dd 1024                      ; width
    dd 768                       ; height
    dd 32                        ; depth

    align 8
    dw 0                         ; end tag
    dw 0
    dd 8
header_end:

section .text
global _start
extern kernel_main

_start:
    cli
    cld
    mov esp, stack_top

    ; Multiboot2: eax=magic, ebx=info. Pass info pointer as cdecl arg.
    push ebx
    call kernel_main
    add esp, 4

.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:
