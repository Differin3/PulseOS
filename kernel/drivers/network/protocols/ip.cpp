#include "ip.h"
#include "arp.h"
#include "ethernet.h"
#include "route.h"
#include "../dhcp/dhcp.h"
#include "../dns/dns.h"
#include "../core/netif.h"
#include "../nic.h"
#include "drivers/timer/pit.h"
#include <stddef.h>

static uint32_t our_ip_address = 0;
static uint32_t gateway_ip = 0;
static uint32_t subnet_mask = 0xFFFFFF00;
static uint16_t ip_id_counter = 1;

struct ip_reass_entry {
    bool used;
    uint32_t src;
    uint32_t dest;
    uint16_t id;
    uint8_t protocol;
    uint8_t buffer[IP_REASS_BUF];
    uint16_t received_len;
    uint16_t total_len; // 0 until last fragment
    uint32_t bitmap;    // coarse coverage in 256-byte blocks
    uint32_t deadline_ms;
};

static struct ip_reass_entry reass_table[IP_REASS_MAX];
static uint8_t reass_deliver_buf[IP_REASS_BUF];

static inline uint16_t htons(uint16_t hostshort) {
    return (uint16_t)(((hostshort & 0xFF) << 8) | ((hostshort >> 8) & 0xFF));
}

static inline uint16_t ntohs(uint16_t netshort) {
    return (uint16_t)(((netshort & 0xFF) << 8) | ((netshort >> 8) & 0xFF));
}

static inline uint32_t htonl(uint32_t hostlong) {
    return ((hostlong & 0xFF) << 24) | ((hostlong & 0xFF00) << 8) |
           ((hostlong & 0xFF0000) >> 8) | ((hostlong & 0xFF000000) >> 24);
}

static inline uint32_t ntohl(uint32_t netlong) {
    return ((netlong & 0xFF) << 24) | ((netlong & 0xFF00) << 8) |
           ((netlong & 0xFF0000) >> 8) | ((netlong & 0xFF000000) >> 24);
}

uint16_t ip_checksum(const void* data, size_t len) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        sum += ((uint16_t)bytes[i] << 8) | bytes[i + 1];
    }
    if (len & 1u) {
        sum += (uint16_t)bytes[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

void ip_reassembly_init(void) {
    for (int i = 0; i < IP_REASS_MAX; i++) {
        reass_table[i].used = false;
    }
}

void ip_reassembly_gc(void) {
    uint32_t now = timer_ms();
    for (int i = 0; i < IP_REASS_MAX; i++) {
        if (reass_table[i].used && (int32_t)(now - reass_table[i].deadline_ms) >= 0) {
            reass_table[i].used = false;
        }
    }
}

int ip_create_packet_ex(void* buffer, size_t buffer_size,
                        uint32_t src_ip, uint32_t dest_ip,
                        uint8_t protocol, uint16_t id, uint16_t flags_frag,
                        const void* payload, size_t payload_size) {
    if (!buffer || buffer_size < sizeof(struct ip_header) + payload_size) {
        return -1;
    }
    struct ip_header* header = (struct ip_header*)buffer;
    header->version_ihl = 0x45;
    header->tos = 0;
    uint16_t total_len = (uint16_t)(sizeof(struct ip_header) + payload_size);
    header->total_length = htons(total_len);
    header->id = htons(id);
    header->flags_fragment = htons(flags_frag);
    header->ttl = 64;
    header->protocol = protocol;
    header->src_ip = htonl(src_ip);
    header->dest_ip = htonl(dest_ip);
    header->checksum = 0;
    if (payload && payload_size > 0) {
        uint8_t* payload_ptr = (uint8_t*)buffer + sizeof(struct ip_header);
        const uint8_t* src = (const uint8_t*)payload;
        for (size_t i = 0; i < payload_size; i++) payload_ptr[i] = src[i];
    }
    uint16_t csum = ip_checksum(header, sizeof(struct ip_header));
    header->checksum = htons(csum);
    return (int)total_len;
}

int ip_create_packet(void* buffer, size_t buffer_size,
                    uint32_t src_ip, uint32_t dest_ip,
                    uint8_t protocol, const void* payload, size_t payload_size) {
    uint16_t id = ip_id_counter++;
    if (ip_id_counter == 0) ip_id_counter = 1;
    return ip_create_packet_ex(buffer, buffer_size, src_ip, dest_ip, protocol,
                               id, IP_FLAG_DF, payload, payload_size);
}

int ip_parse_header(const void* packet, size_t packet_size,
                   struct ip_header* header, const void** payload, size_t* payload_size) {
    if (!packet || packet_size < sizeof(struct ip_header)) return -1;
    const struct ip_header* hdr = (const struct ip_header*)packet;
    if ((hdr->version_ihl >> 4) != 4) return -1;
    uint8_t ihl = (uint8_t)((hdr->version_ihl & 0x0F) * 4);
    if (ihl < 20 || packet_size < ihl) return -1;
    uint16_t total_len = ntohs(hdr->total_length);
    if (total_len < ihl || total_len > packet_size) return -1;

    uint16_t saved_checksum = hdr->checksum;
    struct ip_header verify = *hdr;
    verify.checksum = 0;
    uint16_t calc_checksum = ip_checksum(&verify, ihl);
    if (calc_checksum != ntohs(saved_checksum)) return -1;

    if (header) {
        *header = *hdr;
        header->src_ip = ntohl(hdr->src_ip);
        header->dest_ip = ntohl(hdr->dest_ip);
        header->total_length = total_len;
        header->id = ntohs(hdr->id);
        header->flags_fragment = ntohs(hdr->flags_fragment);
        header->checksum = ntohs(saved_checksum);
    }
    if (payload) *payload = (const uint8_t*)packet + ihl;
    if (payload_size) *payload_size = total_len - ihl;
    return 0;
}

uint8_t ip_get_protocol(const void* packet, size_t packet_size) {
    if (!packet || packet_size < sizeof(struct ip_header)) return 0;
    return ((const struct ip_header*)packet)->protocol;
}

void ip_set_our_ip(uint32_t ip) {
    our_ip_address = ip;
    arp_set_our_ip(ip);
    struct netif* nif = netif_default();
    if (nif) {
        nif->ip = ip;
        route_update_connected(nif);
    }
}

uint32_t ip_get_our_ip() { return our_ip_address; }

void ip_set_gateway(uint32_t gateway) {
    gateway_ip = gateway;
    struct netif* nif = netif_default();
    if (nif) {
        nif->gateway = gateway;
        route_set_default(gateway, nif);
    }
}

uint32_t ip_get_gateway() { return gateway_ip; }

void ip_set_subnet_mask(uint32_t mask) {
    subnet_mask = mask;
    struct netif* nif = netif_default();
    if (nif) {
        nif->netmask = mask;
        route_update_connected(nif);
    }
}

uint32_t ip_get_subnet_mask() { return subnet_mask; }

static void ip_format_octet(uint8_t octet, char* buf, int* pos) {
    if (octet >= 100) buf[(*pos)++] = (char)('0' + (octet / 100));
    if (octet >= 10) buf[(*pos)++] = (char)('0' + ((octet / 10) % 10));
    buf[(*pos)++] = (char)('0' + (octet % 10));
}

void ip_format_address(uint32_t ip, char* buf, size_t buflen) {
    if (!buf || buflen < 8) return;
    int p = 0;
    ip_format_octet((ip >> 24) & 0xFF, buf, &p);
    buf[p++] = '.';
    ip_format_octet((ip >> 16) & 0xFF, buf, &p);
    buf[p++] = '.';
    ip_format_octet((ip >> 8) & 0xFF, buf, &p);
    buf[p++] = '.';
    ip_format_octet(ip & 0xFF, buf, &p);
    buf[p] = 0;
}

int ip_parse_address(const char* str, size_t len, uint32_t* out) {
    if (!str || !out) return -1;
    uint32_t result = 0;
    int octet = 0;
    uint32_t current = 0;
    bool has_digit = false;
    for (size_t i = 0; i <= len; i++) {
        char c = (i < len) ? str[i] : '.';
        if (c >= '0' && c <= '9') {
            current = current * 10 + (uint32_t)(c - '0');
            if (current > 255) return -1;
            has_digit = true;
        } else if (c == '.' || c == ' ' || c == 0) {
            if (!has_digit) return -1;
            result = (result << 8) | current;
            current = 0;
            has_digit = false;
            octet++;
            if (c == ' ' || c == 0) break;
        } else {
            return -1;
        }
    }
    if (octet != 4) return -1;
    *out = result;
    return 0;
}

int ip_parse_address_token(const char* str, size_t len, size_t* pos, uint32_t* out) {
    if (!str || !out || !pos) return -1;
    while (*pos < len && str[*pos] == ' ') (*pos)++;
    if (*pos >= len) return -1;
    size_t start = *pos;
    while (*pos < len && str[*pos] != ' ') (*pos)++;
    return ip_parse_address(str + start, *pos - start, out);
}

void network_apply_config(uint32_t ip, uint32_t mask, uint32_t gateway, uint32_t dns) {
    if (ip != 0) ip_set_our_ip(ip);
    if (mask != 0) ip_set_subnet_mask(mask);
    if (gateway != 0) ip_set_gateway(gateway);
    if (dns != 0) dns_set_server(dns);
}

uint32_t ip_resolve_next_hop(uint32_t dest_ip) {
    uint32_t next = dest_ip;
    struct netif* nif = 0;
    if (route_lookup(dest_ip, &next, &nif) == 0) return next;
    if (dest_ip == 0xFFFFFFFF) return dest_ip;
    uint32_t our_ip = ip_get_our_ip();
    if (our_ip == 0) return dest_ip;
    uint32_t mask = ip_get_subnet_mask();
    if ((dest_ip & mask) == (our_ip & mask)) return dest_ip;
    uint32_t gateway = ip_get_gateway();
    return gateway ? gateway : dest_ip;
}

bool ip_is_local_dest(uint32_t dest_ip) {
    if (dest_ip == 0xFFFFFFFF || dest_ip == 0) return true;
    uint32_t our_ip = ip_get_our_ip();
    if (our_ip != 0 && dest_ip == our_ip) return true;
    return dhcp_accepts_dest_ip(dest_ip);
}

static int ip_send_frame(struct netif* nif, uint32_t next_hop, const void* ip_pkt, size_t ip_len) {
    uint8_t dest_mac[6];
    if (next_hop == 0xFFFFFFFF) {
        for (int i = 0; i < 6; i++) dest_mac[i] = 0xFF;
    } else if (arp_resolve(next_hop, dest_mac, 3000) != 0) {
        return -1;
    }
    uint8_t our_mac[6];
    if (nif) netif_get_mac(nif, our_mac);
    else nic_get_mac(our_mac);
    uint8_t frame[sizeof(struct ethernet_header) + 1600];
    if (ip_len + sizeof(struct ethernet_header) > sizeof(frame)) return -1;
    int frame_len = ethernet_create_frame(frame, sizeof(frame), dest_mac, our_mac,
                                          ETH_TYPE_IPV4, ip_pkt, ip_len);
    if (frame_len < 0) return -1;
    if (nif) return netif_send(nif, frame, (size_t)frame_len);
    return nic_send_packet(frame, (size_t)frame_len);
}

int ip_output(uint32_t dest_ip, uint8_t protocol, const void* payload, size_t payload_size) {
    if (!payload && payload_size) return -1;
    uint32_t src_ip = ip_get_our_ip();
    uint32_t next_hop = dest_ip;
    struct netif* nif = 0;
    if (route_lookup(dest_ip, &next_hop, &nif) != 0) {
        nif = netif_default();
        next_hop = ip_resolve_next_hop(dest_ip);
    }
    uint16_t mtu = nif ? nif->mtu : NETIF_MTU;
    if (mtu < 576) mtu = 576;
    size_t max_payload = (size_t)mtu - sizeof(struct ip_header);
    // Fragment payload on 8-byte boundaries
    max_payload &= ~(size_t)7;
    if (max_payload < 8) return -1;

    uint16_t id = ip_id_counter++;
    if (ip_id_counter == 0) ip_id_counter = 1;

    if (payload_size <= max_payload) {
        uint8_t ip_buffer[1600];
        int ip_len = ip_create_packet_ex(ip_buffer, sizeof(ip_buffer), src_ip, dest_ip,
                                         protocol, id, 0, payload, payload_size);
        if (ip_len < 0) return -1;
        return ip_send_frame(nif, next_hop, ip_buffer, (size_t)ip_len);
    }

    size_t offset = 0;
    const uint8_t* src = (const uint8_t*)payload;
    while (offset < payload_size) {
        size_t chunk = payload_size - offset;
        if (chunk > max_payload) chunk = max_payload;
        bool more = (offset + chunk) < payload_size;
        uint16_t flags_frag = (uint16_t)((offset / 8) & IP_OFFMASK);
        if (more) flags_frag |= IP_FLAG_MF;
        uint8_t ip_buffer[1600];
        int ip_len = ip_create_packet_ex(ip_buffer, sizeof(ip_buffer), src_ip, dest_ip,
                                         protocol, id, flags_frag, src + offset, chunk);
        if (ip_len < 0) return -1;
        if (ip_send_frame(nif, next_hop, ip_buffer, (size_t)ip_len) != 0) return -1;
        offset += chunk;
    }
    return 0;
}

int ip_input_fragment(const void* packet, size_t packet_size,
                      struct ip_header* header,
                      const void** payload_out, size_t* payload_len_out) {
    struct ip_header hdr;
    const void* frag_payload;
    size_t frag_len;
    if (ip_parse_header(packet, packet_size, &hdr, &frag_payload, &frag_len) != 0) {
        return -1;
    }
    uint16_t frag = hdr.flags_fragment;
    uint16_t offset = (uint16_t)((frag & IP_OFFMASK) * 8);
    bool mf = (frag & IP_FLAG_MF) != 0;
    if (!mf && offset == 0) {
        return -1; // not a fragment
    }

    int slot = -1;
    for (int i = 0; i < IP_REASS_MAX; i++) {
        if (reass_table[i].used &&
            reass_table[i].src == hdr.src_ip &&
            reass_table[i].dest == hdr.dest_ip &&
            reass_table[i].id == hdr.id &&
            reass_table[i].protocol == hdr.protocol) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < IP_REASS_MAX; i++) {
            if (!reass_table[i].used) { slot = i; break; }
        }
        if (slot < 0) return -1;
        reass_table[slot].used = true;
        reass_table[slot].src = hdr.src_ip;
        reass_table[slot].dest = hdr.dest_ip;
        reass_table[slot].id = hdr.id;
        reass_table[slot].protocol = hdr.protocol;
        reass_table[slot].received_len = 0;
        reass_table[slot].total_len = 0;
        reass_table[slot].bitmap = 0;
        for (size_t i = 0; i < IP_REASS_BUF; i++) reass_table[slot].buffer[i] = 0;
    }
    reass_table[slot].deadline_ms = timer_ms() + IP_REASS_TIMEOUT_MS;

    if (offset + frag_len > IP_REASS_BUF) {
        reass_table[slot].used = false;
        return -1;
    }
    const uint8_t* src = (const uint8_t*)frag_payload;
    for (size_t i = 0; i < frag_len; i++) {
        reass_table[slot].buffer[offset + i] = src[i];
    }
    reass_table[slot].received_len = (uint16_t)(reass_table[slot].received_len + frag_len);
    if (!mf) {
        reass_table[slot].total_len = (uint16_t)(offset + frag_len);
    }

    if (reass_table[slot].total_len == 0 ||
        reass_table[slot].received_len < reass_table[slot].total_len) {
        return 1;
    }

    size_t total = reass_table[slot].total_len;
    for (size_t i = 0; i < total; i++) {
        reass_deliver_buf[i] = reass_table[slot].buffer[i];
    }
    if (header) {
        *header = hdr;
        header->flags_fragment = 0;
    }
    if (payload_out) *payload_out = reass_deliver_buf;
    if (payload_len_out) *payload_len_out = total;
    reass_table[slot].used = false;
    return 0;
}
