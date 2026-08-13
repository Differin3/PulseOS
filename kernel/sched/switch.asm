BITS 32

section .text
global sched_switch

; void sched_switch(uint32_t** old_esp, uint32_t* new_esp);
; cdecl: [esp+4]=old_esp**, [esp+8]=new_esp*
sched_switch:
    mov eax, [esp + 4]      ; old_esp**
    mov edx, [esp + 8]      ; new_esp*

    push ebx
    push esi
    push edi
    push ebp

    mov [eax], esp          ; *old_esp = current ESP
    mov esp, edx            ; load new task ESP

    pop ebp
    pop edi
    pop esi
    pop ebx
    ret
