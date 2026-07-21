#include "idt.h"

extern "C" {
    extern void idt_load(uint32_t);
    extern void keyboard_handler();
    extern void default_handler();
    extern void syscall_handler_asm();
}

#define IDT_ENTRIES 256
idt_entry idt[IDT_ENTRIES];
idt_ptr idtp;

// Установка записи IDT (32-bit)
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low  = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].selector  = sel;
    idt[num].zero      = 0;
    idt[num].flags     = flags;
}

// Инициализация IDT
void idt_init() {
    idtp.limit = (sizeof(idt_entry) * IDT_ENTRIES) - 1;
    idtp.base  = (uint32_t)&idt;
    
    // Обработчик по умолчанию
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, (uint32_t)default_handler, 0x08, 0x8E);
    }
    
    // Клавиатура (IRQ1 = 33)
    idt_set_gate(33, (uint32_t)keyboard_handler, 0x08, 0x8E);
    
    // Системные вызовы (int 0x80 = 128)
    // Флаги: 0xEE = 0b11101110 (present=1, DPL=3 для пользовательского режима, тип=0xE=interrupt gate)
    idt_set_gate(0x80, (uint32_t)syscall_handler_asm, 0x08, 0xEE);
    
    // Загрузить IDT
    idt_load((uint32_t)&idtp);
}

