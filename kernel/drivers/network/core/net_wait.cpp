#include "net_wait.h"
#include "net_rx.h"
#include "drivers/timer/pit.h"
#include "sched/task.h"

void net_wait_ms(uint32_t ms) {
    uint32_t start = timer_ms();
    uint32_t start_j = timer_jiffies();

    /* Boot DHCP runs before sti: jiffies stay 0, so
       while (timer_ms_since(start) < ms) never ends. ACK is handled
       inside net_process() here, then we hang forever (H1). */
    if (start_j == 0 && start == 0) {
        uint32_t iters = ms * 8000u;
        if (iters < 1000u) iters = 1000u;
        for (uint32_t n = 0; n < iters; n++) {
            if ((n & 0xFF) == 0) net_process();
            for (volatile int i = 0; i < 50; i++) {}
        }
        return;
    }

    if (!sched_ready()) {
        while (timer_ms_since(start) < ms) {
            net_process();
            asm volatile ("hlt");
            for (volatile int i = 0; i < 1000; i++) {}
        }
        return;
    }

    uint32_t deadline = start + ms;
    while (timer_ms_since(start) < ms) {
        net_process();
        if (timer_ms_since(start) >= ms) break;
        task_block_timeout(WAIT_NET, deadline);
    }
}
