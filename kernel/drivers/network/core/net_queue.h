#ifndef NET_QUEUE_H
#define NET_QUEUE_H

#include "skb.h"
#include <stdbool.h>

#define NET_QUEUE_SIZE 32

struct net_queue {
    struct skb* slots[NET_QUEUE_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t drops;
};

void net_queue_init(struct net_queue* q);
bool net_queue_push(struct net_queue* q, struct skb* skb);
struct skb* net_queue_pop(struct net_queue* q);
bool net_queue_empty(const struct net_queue* q);

#endif
