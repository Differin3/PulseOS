BITS 32

; Загрузка IDT (32-bit)
global idt_load
idt_load:
    mov eax, [esp + 4]
    lidt [eax]
    ret

; Обработчик клавиатуры
global keyboard_handler
extern keyboard_handler_main
keyboard_handler:
    pushad
    call keyboard_handler_main
    ; EOI для мастер и для slave, чтобы исключить блокировку
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    popad
    iret

; Обработчик системных вызовов (int 0x80)
; Параметры: eax = номер вызова, ebx = arg1, ecx = arg2, edx = arg3, esi = arg4
global syscall_handler_asm
extern syscall_handler
syscall_handler_asm:
    ; Сохраняем регистры до pushad (чтобы использовать их значения)
    push esi  ; arg4
    push edx  ; arg3
    push ecx  ; arg2
    push ebx  ; arg1
    push eax  ; arg0 (номер системного вызова)
    
    ; Теперь сохраняем все регистры
    pushad
    
    ; Вызываем C обработчик (аргумент - указатель на структуру на стеке)
    ; Структура находится по смещению 32 от текущего esp (8 регистров * 4)
    mov eax, esp
    add eax, 32
    push eax
    call syscall_handler
    add esp, 4  ; Убираем аргумент
    
    ; Сохраняем результат в место где был сохранен EAX в pushad
    ; После pushad EAX находится по смещению 28 от текущего esp
    mov [esp + 28], eax
    
    ; Восстанавливаем регистры (результат уже в eax)
    popad
    
    ; Восстанавливаем стек (убираем структуру аргументов)
    add esp, 20  ; 5 * 4 байта (arg0-arg4)
    
    iret

; Обработчик по умолчанию для всех прерываний
global default_handler
default_handler:
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    iret

