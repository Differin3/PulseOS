#ifndef PIT_H
#define PIT_H

#include <stdint.h>

#define PIT_HZ 100

void pit_init(void);
void interrupts_enable(void);
uint32_t timer_jiffies(void);
uint32_t timer_ms(void);
void timer_delay_ms(uint32_t ms);
uint32_t timer_ms_since(uint32_t start_ms);

#endif
