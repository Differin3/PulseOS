#include "skb.h"

static struct skb skb_pool[SKB_POOL_SIZE];

void skb_pool_init(void) {
    for (int i = 0; i < SKB_POOL_SIZE; i++) {
        skb_pool[i].in_use = false;
        skb_pool[i].len = 0;
        skb_pool[i].netif = 0;
        skb_pool[i].protocol = 0;
    }
}

struct skb* skb_alloc(void) {
    for (int i = 0; i < SKB_POOL_SIZE; i++) {
        if (!skb_pool[i].in_use) {
            skb_pool[i].in_use = true;
            skb_pool[i].len = 0;
            skb_pool[i].netif = 0;
            skb_pool[i].protocol = 0;
            return &skb_pool[i];
        }
    }
    return 0;
}

void skb_free(struct skb* skb) {
    if (!skb) return;
    skb->in_use = false;
    skb->len = 0;
    skb->netif = 0;
    skb->protocol = 0;
}

int skb_copy_from(struct skb* skb, const void* data, size_t len) {
    if (!skb || !data || len == 0 || len > SKB_DATA_SIZE) return -1;
    const uint8_t* src = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        skb->data[i] = src[i];
    }
    skb->len = len;
    return 0;
}
