#include "udp.h"
#include "ip.h"
#include "ethernet.h"
#include "arp.h"
#include "../nic.h"  // для ETH_MAX_PACKET_SIZE и nic_get_mac
#include "../socket.h"
#include "../core/net_ports.h"
#include "serial_log.h"
#include <stddef.h>
#include <stdbool.h>

// Преобразование host byte order в network byte order (big-endian)
static inline uint16_t htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort >> 8) & 0xFF);
}

// Преобразование network byte order в host byte order
static inline uint16_t ntohs(uint16_t netshort) {
    return ((netshort & 0xFF) << 8) | ((netshort >> 8) & 0xFF);
}

#define UDP_MAX_LISTEN_PORTS 4
static uint16_t udp_listen_ports[UDP_MAX_LISTEN_PORTS];

int udp_listen_port(uint16_t port) {
    if (port == 0) return -1;
    for (int i = 0; i < UDP_MAX_LISTEN_PORTS; i++) {
        if (udp_listen_ports[i] == port) return 0;
    }
    for (int i = 0; i < UDP_MAX_LISTEN_PORTS; i++) {
        if (udp_listen_ports[i] == 0) {
            udp_listen_ports[i] = port;
            net_ports_register(NET_PROTO_UDP, NETPORT_LISTEN,
                               ip_get_our_ip(), port, 0, 0,
                               -1, -1, "udplisten");
            return 0;
        }
    }
    return -1;
}

void udp_unlisten_port(uint16_t port) {
    for (int i = 0; i < UDP_MAX_LISTEN_PORTS; i++) {
        if (udp_listen_ports[i] == port) {
            udp_listen_ports[i] = 0;
            net_ports_release_listen(NET_PROTO_UDP, port);
            return;
        }
    }
}

static bool udp_is_listening(uint16_t port) {
    for (int i = 0; i < UDP_MAX_LISTEN_PORTS; i++) {
        if (udp_listen_ports[i] == port) return true;
    }
    return false;
}

// Создать UDP пакет
uint16_t udp_checksum(uint32_t src_ip, uint32_t dest_ip,
                      const struct udp_header* header, size_t udp_len) {
    struct {
        uint32_t src_ip;
        uint32_t dest_ip;
        uint8_t zero;
        uint8_t protocol;
        uint16_t udp_len;
    } __attribute__((packed)) pseudo_header;

    pseudo_header.src_ip = ((src_ip & 0xFF) << 24) | ((src_ip & 0xFF00) << 8) |
                           ((src_ip & 0xFF0000) >> 8) | ((src_ip & 0xFF000000) >> 24);
    pseudo_header.dest_ip = ((dest_ip & 0xFF) << 24) | ((dest_ip & 0xFF00) << 8) |
                            ((dest_ip & 0xFF0000) >> 8) | ((dest_ip & 0xFF000000) >> 24);
    pseudo_header.zero = 0;
    pseudo_header.protocol = IP_PROTOCOL_UDP;
    pseudo_header.udp_len = htons((uint16_t)udp_len);

    uint32_t sum = 0;
    const uint8_t* bytes = (const uint8_t*)&pseudo_header;
    for (size_t i = 0; i < sizeof(pseudo_header); i += 2) {
        if (i + 1 < sizeof(pseudo_header)) {
            sum += ((uint16_t)bytes[i] << 8) | bytes[i + 1];
        } else {
            sum += (uint16_t)bytes[i] << 8;
        }
    }

    bytes = (const uint8_t*)header;
    for (size_t i = 0; i < udp_len; i += 2) {
        if (i + 1 < udp_len) {
            sum += ((uint16_t)bytes[i] << 8) | bytes[i + 1];
        } else {
            sum += (uint16_t)bytes[i] << 8;
        }
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

// Создать UDP пакет
int udp_create_packet(void* buffer, size_t buffer_size,
                     uint16_t src_port, uint16_t dest_port,
                     const void* payload, size_t payload_size) {
    if (!buffer || buffer_size < sizeof(struct udp_header) + payload_size) {
        return -1;
    }
    
    struct udp_header* header = (struct udp_header*)buffer;
    
    // Порты и длина в network byte order (big-endian)
    header->src_port = htons(src_port);
    header->dest_port = htons(dest_port);
    header->length = htons(sizeof(struct udp_header) + payload_size);
    header->checksum = 0; // Упрощенно, не вычисляем checksum
    
    // Копируем payload
    if (payload && payload_size > 0) {
        uint8_t* payload_ptr = (uint8_t*)buffer + sizeof(struct udp_header);
        const uint8_t* src = (const uint8_t*)payload;
        for (size_t i = 0; i < payload_size; i++) {
            payload_ptr[i] = src[i];
        }
    }
    
    return sizeof(struct udp_header) + payload_size;
}

// Парсить UDP заголовок
int udp_parse_header(const void* packet, size_t packet_size,
                    struct udp_header* header, const void** payload, size_t* payload_size) {
    if (!packet || packet_size < sizeof(struct udp_header)) {
        return -1;
    }
    
    const struct udp_header* hdr = (const struct udp_header*)packet;
    
    if (header) {
        *header = *hdr;
    }
    
    if (payload) {
        *payload = (const uint8_t*)packet + sizeof(struct udp_header);
    }
    
    if (payload_size) {
        *payload_size = packet_size - sizeof(struct udp_header);
    }
    
    return 0;
}

// Отправить UDP пакет через IP
int udp_send(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port,
             const void* payload, size_t payload_size) {
    if (payload_size > UDP_MAX_PAYLOAD) {
        log_fmt3(LOG_ERR, "udp", "payload too large", "size", (uint32_t)payload_size, "max", (uint32_t)UDP_MAX_PAYLOAD, "port", (uint32_t)dest_port);
        return -1;
    }
    
    // Создаем UDP пакет
    uint8_t udp_buffer[sizeof(struct udp_header) + UDP_MAX_PAYLOAD];
    int udp_len = udp_create_packet(udp_buffer, sizeof(udp_buffer),
                                    src_port, dest_port, payload, payload_size);
    if (udp_len < 0) return -1;

    uint32_t src_ip = ip_get_our_ip();
    struct udp_header* udp_hdr = (struct udp_header*)udp_buffer;
    uint16_t csum = udp_checksum(src_ip, dest_ip, udp_hdr, (size_t)udp_len);
    if (csum == 0) {
        csum = 0xFFFF;
    }
    udp_hdr->checksum = htons(csum);

    int result = ip_output(dest_ip, IP_PROTOCOL_UDP, udp_buffer, (size_t)udp_len);
    if (result < 0) {
        log_fmt3(LOG_ERR, "udp", "ip_output failed", "len", (uint32_t)udp_len, "dport", (uint32_t)dest_port, "sport", (uint32_t)src_port);
        return -1;
    }
    log_fmt3(LOG_DBG, "udp", "sent", "len", (uint32_t)udp_len, "dport", (uint32_t)dest_port, "sport", (uint32_t)src_port);
    return 0;
}

// Обработать входящий UDP пакет
void udp_handle_packet(uint32_t src_ip, uint32_t dest_ip,
                      const void* ip_payload, size_t payload_size) {
    if (!ip_payload || payload_size < sizeof(struct udp_header)) {
        return;
    }

    struct udp_header udp_hdr;
    const void* udp_payload;
    size_t udp_payload_size;

    if (udp_parse_header(ip_payload, payload_size, &udp_hdr, &udp_payload, &udp_payload_size) != 0) {
        return;
    }

    if (udp_hdr.checksum != 0) {
        uint16_t saved = udp_hdr.checksum;
        struct udp_header verify = udp_hdr;
        verify.checksum = 0;
        size_t udp_len = sizeof(struct udp_header) + udp_payload_size;
        uint16_t calc = udp_checksum(src_ip, dest_ip, &verify, udp_len);
        if (calc == 0) {
            calc = 0xFFFF;
        }
        if (calc != saved) {
            return;
        }
    }

    (void)udp_payload;
    uint16_t src_port = ntohs(udp_hdr.src_port);
    uint16_t dst_port = ntohs(udp_hdr.dest_port);

    if (socket_udp_deliver(dst_port, src_ip, src_port, udp_payload, udp_payload_size)) {
        return;
    }

    if (udp_is_listening(dst_port) && udp_payload_size > 0) {
        udp_send(src_ip, dst_port, src_port, udp_payload, udp_payload_size);
    }
}
