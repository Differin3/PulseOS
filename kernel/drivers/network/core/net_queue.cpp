#include "net_queue.h"

void net_queue_init(struct net_queue* q) {
    if (!q) return;
    q->head = 0;
    q->tail = 0;
    q->drops = 0;
    for (int i = 0; i < NET_QUEUE_SIZE; i++) {
        q->slots[i] = 0;
    }
}

bool net_queue_push(struct net_queue* q, struct skb* skb) {
    if (!q || !skb) return false;
    uint32_t next = (q->tail + 1) % NET_QUEUE_SIZE;
    if (next == q->head) {
        q->drops++;
        return false;
    }
    q->slots[q->tail] = skb;
    q->tail = next;
    return true;
}

struct skb* net_queue_pop(struct net_queue* q) {
    if (!q || q->head == q->tail) return 0;
    struct skb* skb = q->slots[q->head];
    q->slots[q->head] = 0;
    q->head = (q->head + 1) % NET_QUEUE_SIZE;
    return skb;
}

bool net_queue_empty(const struct net_queue* q) {
    if (!q) return true;
    return q->head == q->tail;
}
