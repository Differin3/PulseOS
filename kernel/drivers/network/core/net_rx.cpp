#include "net_rx.h"
#include "netif.h"
#include "skb.h"
#include "../protocols/ethernet.h"
#include "../protocols/arp.h"
#include "../protocols/ip.h"
#include "../protocols/udp.h"
#include "../protocols/tcp.h"
#include "../protocols/icmp.h"
#include "../dns/dns.h"
#include "../dhcp/dhcp.h"
#include "serial_log.h"

static inline uint16_t ntohs(uint16_t netshort) {
    return (uint16_t)(((netshort & 0xFF) << 8) | ((netshort >> 8) & 0xFF));
}

extern void udp_handle_packet(uint32_t src_ip, uint32_t dest_ip,
                              const void* ip_payload, size_t payload_size);
extern void dhcp_handle_packet(uint32_t src_ip, uint16_t src_port,
                               const void* udp_payload, size_t payload_size);
extern void tcp_handle_packet(uint32_t src_ip, uint32_t dest_ip,
                              const void* ip_payload, size_t payload_size);
extern void dns_handle_request(uint32_t src_ip, uint16_t src_port,
                               const void* udp_payload, size_t payload_size);
extern void dns_handle_response(const void* udp_payload, size_t payload_size);
extern int udp_parse_header(const void* packet, size_t packet_size,
                            struct udp_header* header, const void** payload, size_t* payload_size);
extern void tcp_init(void);
extern void dns_init(void);
extern void arp_init(void);
extern void socket_init(void);
extern void route_init(void);
extern void ip_reassembly_init(void);

static void net_handle_ipv4(struct netif* nif, const uint8_t* src_mac,
                            const void* payload, size_t payload_size) {
    (void)nif;
    struct ip_header ip_hdr;
    const void* ip_payload = 0;
    size_t ip_payload_size = 0;

    const void* reassembled = 0;
    size_t reassembled_len = 0;
    int frag_rc = ip_input_fragment(payload, payload_size, &ip_hdr, &reassembled, &reassembled_len);
    if (frag_rc == 1) {
        return; // waiting for more fragments
    }
    if (frag_rc == 0) {
        ip_payload = reassembled;
        ip_payload_size = reassembled_len;
    } else if (ip_parse_header(payload, payload_size, &ip_hdr, &ip_payload, &ip_payload_size) != 0) {
        log_msg(LOG_ERR, "net", "ip parse fail");
        return;
    }

    if (ip_hdr.src_ip != 0 && ip_hdr.src_ip != 0xFFFFFFFF) {
        arp_add_entry(ip_hdr.src_ip, src_mac);
    }
    if (!ip_is_local_dest(ip_hdr.dest_ip)) {
        log_ip(LOG_INFO, "net", "drop dest", ip_hdr.dest_ip);
        return;
    }

    if (ip_hdr.protocol == IP_PROTOCOL_UDP) {
        udp_handle_packet(ip_hdr.src_ip, ip_hdr.dest_ip, ip_payload, ip_payload_size);
        struct udp_header udp_hdr;
        const void* udp_payload;
        size_t udp_payload_size;
        if (udp_parse_header(ip_payload, ip_payload_size, &udp_hdr, &udp_payload, &udp_payload_size) == 0) {
            uint16_t dest_port = ntohs(udp_hdr.dest_port);
            uint16_t src_port = ntohs(udp_hdr.src_port);
            if (dest_port == DNS_PORT) {
                dns_handle_request(ip_hdr.src_ip, src_port, udp_payload, udp_payload_size);
            } else if (dest_port == DNS_CLIENT_PORT) {
                dns_handle_response(udp_payload, udp_payload_size);
            } else if (dest_port == 68) {
                dhcp_handle_packet(ip_hdr.src_ip, src_port, udp_payload, udp_payload_size);
            }
        }
    } else if (ip_hdr.protocol == IP_PROTOCOL_TCP) {
        tcp_handle_packet(ip_hdr.src_ip, ip_hdr.dest_ip, ip_payload, ip_payload_size);
    } else if (ip_hdr.protocol == IP_PROTOCOL_ICMP) {
        icmp_handle_packet(ip_hdr.src_ip, ip_payload, ip_payload_size);
    }
}

static void net_handle_frame(struct netif* nif, const void* frame, size_t len) {
    struct ethernet_header eth_header;
    const void* payload;
    size_t payload_size;
    if (ethernet_parse_header(frame, len, &eth_header, &payload, &payload_size) != 0) {
        log_msg(LOG_ERR, "net", "eth parse fail");
        return;
    }
    uint16_t protocol_type = ntohs(eth_header.type);
    if (protocol_type == ETH_TYPE_ARP) {
        if (payload_size >= sizeof(struct arp_packet)) {
            arp_handle_packet((const struct arp_packet*)payload, payload_size);
        }
    } else if (protocol_type == ETH_TYPE_IPV4) {
        net_handle_ipv4(nif, eth_header.src_mac, payload, payload_size);
    }
}

void net_stack_init(void) {
    route_init();
    ip_reassembly_init();
    arp_init();
    dns_init();
    tcp_init();
    socket_init();
}

void net_process(void) {
    int n = netif_count();
    for (int i = 0; i < n; i++) {
        struct netif* nif = netif_get(i);
        if (!nif || !nif->up) continue;
        netif_poll(nif);

        int guard = 0;
        while (!net_queue_empty(&nif->rx_queue) && guard < 32) {
            guard++;
            struct skb* skb = net_queue_pop(&nif->rx_queue);
            if (!skb) break;
            net_handle_frame(nif, skb->data, skb->len);
            skb_free(skb);
        }
    }
    ip_reassembly_gc();
}
