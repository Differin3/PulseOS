#include "pit.h"
#include "drivers/pic/pic.h"
#include "sched/task.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_HZ  1193182u

static volatile uint32_t g_jiffies = 0;

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

extern "C" void pit_handler_main(void) {
    g_jiffies++;
    sched_on_tick();
    // Master PIC EOI only (IRQ0)
    asm volatile ("outb %0, %1" : : "a"((uint8_t)0x20), "Nd"((uint16_t)0x20));
}

void pit_init(void) {
    uint32_t divisor = PIT_BASE_HZ / PIT_HZ;
    if (divisor == 0) divisor = 1;
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
    pic_unmask(0);
    // Do not sti here — caller enables interrupts after the rest of early init.
}

void interrupts_enable(void) {
    asm volatile ("sti");
}

uint32_t timer_jiffies(void) {
    return g_jiffies;
}

uint32_t timer_ms(void) {
    // 100 Hz => 10 ms per tick
    return g_jiffies * (1000u / PIT_HZ);
}

void timer_delay_ms(uint32_t ms) {
    if (sched_ready()) {
        task_sleep_ms(ms);
        return;
    }
    uint32_t start = timer_ms();
    while (timer_ms_since(start) < ms) {
        asm volatile ("hlt");
    }
}

uint32_t timer_ms_since(uint32_t start_ms) {
    return timer_ms() - start_ms;
}
