#include "idt.h"

extern "C" {
    extern void idt_load(uint32_t);
    extern void keyboard_handler();
    extern void default_handler();
    extern void syscall_handler_asm();
    extern void pit_handler();
    extern void nic_irq_handler();
    extern void page_fault_handler();
}

#define IDT_ENTRIES 256
idt_entry idt[IDT_ENTRIES];
idt_ptr idtp;

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low  = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].selector  = sel;
    idt[num].zero      = 0;
    idt[num].flags     = flags;
}

void idt_init() {
    idtp.limit = (sizeof(idt_entry) * IDT_ENTRIES) - 1;
    idtp.base  = (uint32_t)&idt;

    // Use the live CS from GRUB/Multiboot — hardcoding 0x08 triple-faults on IRQ
    uint16_t kernel_cs = 0;
    asm volatile ("mov %%cs, %0" : "=r"(kernel_cs));

    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate((uint8_t)i, (uint32_t)default_handler, kernel_cs, 0x8E);
    }

    // IRQ0 PIT = 32
    idt_set_gate(32, (uint32_t)pit_handler, kernel_cs, 0x8E);
    // IRQ1 keyboard = 33
    idt_set_gate(33, (uint32_t)keyboard_handler, kernel_cs, 0x8E);
    // Common NIC lines in QEMU: IRQ9-11 -> 41-43
    idt_set_gate(41, (uint32_t)nic_irq_handler, kernel_cs, 0x8E);
    idt_set_gate(42, (uint32_t)nic_irq_handler, kernel_cs, 0x8E);
    idt_set_gate(43, (uint32_t)nic_irq_handler, kernel_cs, 0x8E);

    // #PF vector 14
    idt_set_gate(14, (uint32_t)page_fault_handler, kernel_cs, 0x8E);

    // Syscall int 0x80 — DPL=3
    idt_set_gate(0x80, (uint32_t)syscall_handler_asm, kernel_cs, 0xEE);

    idt_load((uint32_t)&idtp);
}
