BITS 32

; Multiboot2 header для GRUB (i386)
section .multiboot_header
align 8
header_start:
    dd 0xe85250d6                ; Magic number (multiboot2)
    dd 0                         ; Architecture (i386)
    dd header_end - header_start ; Header length
    dd -(0xe85250d6 + 0 + (header_end - header_start)) ; Checksum

    ; End tag
    dw 0    ; Type
    dw 0    ; Flags
    dd 8    ; Size
header_end:

; Точка входа
section .text
global _start
extern kernel_main

_start:
    cli
    cld

    ; Устанавливаем стек (32-bit)
    mov esp, stack_top

    ; Переходим в ядро C
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

; Стек
section .bss
align 16
stack_bottom:
    resb 16384 ; 16 KB стек
stack_top:

