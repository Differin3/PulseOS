#ifndef NETIF_H
#define NETIF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "net_queue.h"

#define NETIF_MAX 4
#define NETIF_NAME_LEN 8
#define NETIF_MTU 1500

enum netif_hw_type {
    NETIF_HW_NONE = 0,
    NETIF_HW_RTL8139,
    NETIF_HW_PCNET,
    NETIF_HW_VIRTIO
};

struct nic_device;
struct netif;

struct netif_ops {
    int (*send)(struct netif* nif, const void* data, size_t len);
    int (*poll)(struct netif* nif); // drain HW RX into rx_queue
    void (*irq)(struct netif* nif);
};

struct netif_stats {
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_dropped;
    uint32_t tx_errors;
    uint32_t irq_count;
};

struct netif {
    char name[NETIF_NAME_LEN];
    uint8_t mac[6];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint16_t mtu;
    enum netif_hw_type hw_type;
    uint8_t irq_line;
    bool up;
    bool initialized;
    struct nic_device* nic;
    struct netif_ops ops;
    struct net_queue rx_queue;
    struct netif_stats stats;
    void* priv;
};

void netif_init_subsystem(void);
struct netif* netif_alloc(const char* name);
struct netif* netif_default(void);
void netif_set_default(struct netif* nif);
int netif_register(struct netif* nif);
struct netif* netif_get(int index);
int netif_count(void);

int netif_send(struct netif* nif, const void* data, size_t len);
int netif_poll(struct netif* nif);
void netif_handle_irq(struct netif* nif);
int net_rx_enqueue(struct netif* nif, const void* data, size_t len);

void netif_set_addr(struct netif* nif, uint32_t ip, uint32_t mask, uint32_t gw);
void netif_get_mac(struct netif* nif, uint8_t* mac);

#endif
