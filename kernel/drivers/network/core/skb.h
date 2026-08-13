#ifndef NET_SKB_H
#define NET_SKB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SKB_DATA_SIZE 2048
#define SKB_POOL_SIZE 64

struct netif;

struct skb {
    uint8_t data[SKB_DATA_SIZE];
    size_t len;
    struct netif* netif;
    uint16_t protocol; // ethertype host order
    bool in_use;
};

void skb_pool_init(void);
struct skb* skb_alloc(void);
void skb_free(struct skb* skb);
int skb_copy_from(struct skb* skb, const void* data, size_t len);

#endif
