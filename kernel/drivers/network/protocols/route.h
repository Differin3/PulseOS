#ifndef ROUTE_H
#define ROUTE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct netif;

#define ROUTE_MAX 16

struct route_entry {
    uint32_t dest;
    uint32_t mask;
    uint32_t gateway; // 0 = on-link
    struct netif* nif;
    bool valid;
};

void route_init(void);
int route_add(uint32_t dest, uint32_t mask, uint32_t gateway, struct netif* nif);
int route_del(uint32_t dest, uint32_t mask);
void route_set_default(uint32_t gateway, struct netif* nif);
void route_update_connected(struct netif* nif);

// Lookup: fills next_hop and out_if; returns 0 on success
int route_lookup(uint32_t dest_ip, uint32_t* next_hop, struct netif** out_if);

int route_count(void);
const struct route_entry* route_get(int index);

#endif
