#ifndef IP_H
#define IP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define IP_PROTOCOL_ICMP 1
#define IP_PROTOCOL_TCP  6
#define IP_PROTOCOL_UDP  17

#define IP_FLAG_MF   0x2000
#define IP_FLAG_DF   0x4000
#define IP_OFFMASK   0x1FFF

#define IP_REASS_MAX 4
#define IP_REASS_BUF 8192
#define IP_REASS_TIMEOUT_MS 3000

struct netif;

struct ip_header {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_fragment;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dest_ip;
} __attribute__((packed));

int ip_create_packet(void* buffer, size_t buffer_size,
                    uint32_t src_ip, uint32_t dest_ip,
                    uint8_t protocol, const void* payload, size_t payload_size);

int ip_create_packet_ex(void* buffer, size_t buffer_size,
                        uint32_t src_ip, uint32_t dest_ip,
                        uint8_t protocol, uint16_t id, uint16_t flags_frag,
                        const void* payload, size_t payload_size);

int ip_parse_header(const void* packet, size_t packet_size,
                   struct ip_header* header, const void** payload, size_t* payload_size);

uint8_t ip_get_protocol(const void* packet, size_t packet_size);
uint16_t ip_checksum(const void* data, size_t len);

void ip_set_our_ip(uint32_t ip);
uint32_t ip_get_our_ip();
void ip_set_gateway(uint32_t gateway);
uint32_t ip_get_gateway();
void ip_set_subnet_mask(uint32_t mask);
uint32_t ip_get_subnet_mask();

void ip_format_address(uint32_t ip, char* buf, size_t buflen);
int ip_parse_address(const char* str, size_t len, uint32_t* out);
int ip_parse_address_token(const char* str, size_t len, size_t* pos, uint32_t* out);

uint32_t ip_resolve_next_hop(uint32_t dest_ip);
bool ip_is_local_dest(uint32_t dest_ip);
void network_apply_config(uint32_t ip, uint32_t mask, uint32_t gateway, uint32_t dns);

// Unified TX: build IP (+frag) + ARP + Ethernet + netif_send
int ip_output(uint32_t dest_ip, uint8_t protocol, const void* payload, size_t payload_size);

void ip_reassembly_init(void);
void ip_reassembly_gc(void);

// Returns: 0 complete (payload out), 1 incomplete fragment, -1 not fragment / error (use normal parse)
int ip_input_fragment(const void* packet, size_t packet_size,
                      struct ip_header* header,
                      const void** payload_out, size_t* payload_len_out);

#endif
