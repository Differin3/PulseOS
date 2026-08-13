#include "route.h"
#include "../core/netif.h"

static struct route_entry routes[ROUTE_MAX];

void route_init(void) {
    for (int i = 0; i < ROUTE_MAX; i++) {
        routes[i].valid = false;
        routes[i].dest = 0;
        routes[i].mask = 0;
        routes[i].gateway = 0;
        routes[i].nif = 0;
    }
}

static int route_find_slot(void) {
    for (int i = 0; i < ROUTE_MAX; i++) {
        if (!routes[i].valid) return i;
    }
    return -1;
}

int route_add(uint32_t dest, uint32_t mask, uint32_t gateway, struct netif* nif) {
    for (int i = 0; i < ROUTE_MAX; i++) {
        if (routes[i].valid && routes[i].dest == dest && routes[i].mask == mask) {
            routes[i].gateway = gateway;
            routes[i].nif = nif;
            return 0;
        }
    }
    int idx = route_find_slot();
    if (idx < 0) return -1;
    routes[idx].dest = dest;
    routes[idx].mask = mask;
    routes[idx].gateway = gateway;
    routes[idx].nif = nif;
    routes[idx].valid = true;
    return 0;
}

int route_del(uint32_t dest, uint32_t mask) {
    for (int i = 0; i < ROUTE_MAX; i++) {
        if (routes[i].valid && routes[i].dest == dest && routes[i].mask == mask) {
            routes[i].valid = false;
            return 0;
        }
    }
    return -1;
}

void route_set_default(uint32_t gateway, struct netif* nif) {
    route_add(0, 0, gateway, nif);
}

void route_update_connected(struct netif* nif) {
    if (!nif || nif->ip == 0) return;
    uint32_t net = nif->ip & nif->netmask;
    route_add(net, nif->netmask, 0, nif);
    if (nif->gateway) {
        route_set_default(nif->gateway, nif);
    }
}

int route_lookup(uint32_t dest_ip, uint32_t* next_hop, struct netif** out_if) {
    if (dest_ip == 0xFFFFFFFF) {
        if (next_hop) *next_hop = dest_ip;
        if (out_if) *out_if = netif_default();
        return netif_default() ? 0 : -1;
    }

    int best = -1;
    uint32_t best_mask = 0;
    for (int i = 0; i < ROUTE_MAX; i++) {
        if (!routes[i].valid) continue;
        if ((dest_ip & routes[i].mask) == (routes[i].dest & routes[i].mask)) {
            if (best < 0 || routes[i].mask >= best_mask) {
                best = i;
                best_mask = routes[i].mask;
            }
        }
    }

    if (best < 0) {
        struct netif* nif = netif_default();
        if (!nif) return -1;
        if (next_hop) {
            if (nif->gateway) *next_hop = nif->gateway;
            else *next_hop = dest_ip;
        }
        if (out_if) *out_if = nif;
        return 0;
    }

    if (next_hop) {
        if (routes[best].gateway) *next_hop = routes[best].gateway;
        else *next_hop = dest_ip;
    }
    if (out_if) *out_if = routes[best].nif ? routes[best].nif : netif_default();
    return 0;
}

int route_count(void) {
    int n = 0;
    for (int i = 0; i < ROUTE_MAX; i++) {
        if (routes[i].valid) n++;
    }
    return n;
}

const struct route_entry* route_get(int index) {
    int n = 0;
    for (int i = 0; i < ROUTE_MAX; i++) {
        if (!routes[i].valid) continue;
        if (n == index) return &routes[i];
        n++;
    }
    return 0;
}
