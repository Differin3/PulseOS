#ifndef NET_WAIT_H
#define NET_WAIT_H

#include <stdint.h>

// Sleep up to ms while pumping the network stack
void net_wait_ms(uint32_t ms);

#endif
