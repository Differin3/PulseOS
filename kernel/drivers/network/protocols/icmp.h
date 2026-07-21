#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>
#include <stddef.h>

#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_DEST_UNREACH  3
#define ICMP_TYPE_ECHO_REQUEST  8
#define ICMP_TYPE_TIME_EXCEEDED 11

struct icmp_header {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} __attribute__((packed));

// Обработать входящий ICMP пакет
void icmp_handle_packet(uint32_t src_ip, const void* payload, size_t payload_size);

// Ping хоста (блокирующий, polling)
int icmp_ping(uint32_t dest_ip, int count);

#endif
