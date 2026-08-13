#include "netif.h"
#include "skb.h"

static struct netif netifs[NETIF_MAX];
static int netif_used = 0;
static struct netif* default_netif = 0;

static void copy_name(char* dst, const char* src) {
    int i = 0;
    if (!src) src = "eth?";
    while (src[i] && i < NETIF_NAME_LEN - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

void netif_init_subsystem(void) {
    skb_pool_init();
    netif_used = 0;
    default_netif = 0;
    for (int i = 0; i < NETIF_MAX; i++) {
        netifs[i].initialized = false;
        netifs[i].up = false;
        netifs[i].nic = 0;
        netifs[i].priv = 0;
        netifs[i].hw_type = NETIF_HW_NONE;
        netifs[i].irq_line = 0;
        netifs[i].mtu = NETIF_MTU;
        netifs[i].ip = 0;
        netifs[i].netmask = 0xFFFFFF00;
        netifs[i].gateway = 0;
        netifs[i].ops.send = 0;
        netifs[i].ops.poll = 0;
        netifs[i].ops.irq = 0;
        net_queue_init(&netifs[i].rx_queue);
        netifs[i].stats.rx_packets = 0;
        netifs[i].stats.tx_packets = 0;
        netifs[i].stats.rx_bytes = 0;
        netifs[i].stats.tx_bytes = 0;
        netifs[i].stats.rx_dropped = 0;
        netifs[i].stats.tx_errors = 0;
        netifs[i].stats.irq_count = 0;
        for (int j = 0; j < 6; j++) netifs[i].mac[j] = 0;
        netifs[i].name[0] = 0;
    }
}

struct netif* netif_alloc(const char* name) {
    if (netif_used >= NETIF_MAX) return 0;
    struct netif* nif = &netifs[netif_used++];
    copy_name(nif->name, name);
    nif->initialized = true;
    nif->up = false;
    net_queue_init(&nif->rx_queue);
    if (!default_netif) default_netif = nif;
    return nif;
}

struct netif* netif_default(void) {
    return default_netif;
}

void netif_set_default(struct netif* nif) {
    if (nif) default_netif = nif;
}

int netif_register(struct netif* nif) {
    if (!nif || !nif->initialized) return -1;
    nif->up = true;
    if (!default_netif) default_netif = nif;
    return 0;
}

struct netif* netif_get(int index) {
    if (index < 0 || index >= netif_used) return 0;
    return &netifs[index];
}

int netif_count(void) {
    return netif_used;
}

int netif_send(struct netif* nif, const void* data, size_t len) {
    if (!nif || !nif->up || !nif->ops.send || !data || len == 0) return -1;
    int rc = nif->ops.send(nif, data, len);
    if (rc == 0) {
        nif->stats.tx_packets++;
        nif->stats.tx_bytes += (uint32_t)len;
    } else {
        nif->stats.tx_errors++;
    }
    return rc;
}

int netif_poll(struct netif* nif) {
    if (!nif || !nif->up || !nif->ops.poll) return 0;
    return nif->ops.poll(nif);
}

void netif_handle_irq(struct netif* nif) {
    if (!nif || !nif->ops.irq) return;
    nif->stats.irq_count++;
    nif->ops.irq(nif);
}

int net_rx_enqueue(struct netif* nif, const void* data, size_t len) {
    if (!nif || !data || len == 0) return -1;
    struct skb* skb = skb_alloc();
    if (!skb) {
        nif->stats.rx_dropped++;
        return -1;
    }
    if (skb_copy_from(skb, data, len) != 0) {
        skb_free(skb);
        nif->stats.rx_dropped++;
        return -1;
    }
    skb->netif = nif;
    if (!net_queue_push(&nif->rx_queue, skb)) {
        skb_free(skb);
        nif->stats.rx_dropped++;
        return -1;
    }
    nif->stats.rx_packets++;
    nif->stats.rx_bytes += (uint32_t)len;
    return 0;
}

void netif_set_addr(struct netif* nif, uint32_t ip, uint32_t mask, uint32_t gw) {
    if (!nif) return;
    if (ip) nif->ip = ip;
    if (mask) nif->netmask = mask;
    if (gw) nif->gateway = gw;
}

void netif_get_mac(struct netif* nif, uint8_t* mac) {
    if (!nif || !mac) return;
    for (int i = 0; i < 6; i++) mac[i] = nif->mac[i];
}
