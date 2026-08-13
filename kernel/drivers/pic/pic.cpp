#include "pic.h"

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void pic_remap(void) {
    outb(PIC1_COMMAND, 0x11);
    outb(PIC2_COMMAND, 0x11);
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_unmask(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq = (uint8_t)(irq - 8);
        // Cascade IRQ2 must stay unmasked for slave IRQs
        uint8_t master = inb(PIC1_DATA);
        master = (uint8_t)(master & ~(1u << 2));
        outb(PIC1_DATA, master);
    }
    value = inb(port);
    value = (uint8_t)(value & ~(1u << irq));
    outb(port, value);
}

void pic_mask(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq = (uint8_t)(irq - 8);
    }
    value = inb(port);
    value = (uint8_t)(value | (1u << irq));
    outb(port, value);
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}
