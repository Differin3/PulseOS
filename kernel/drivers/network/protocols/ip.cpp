#include "ip.h"
#include "arp.h"
#include "../dhcp/dhcp.h"
#include "../dns/dns.h"
#include <stddef.h>

static uint32_t our_ip_address = 0;
static uint32_t gateway_ip = 0;
static uint32_t subnet_mask = 0xFFFFFF00; // 255.255.255.0 по умолчанию

// Преобразование host byte order в network byte order (big-endian)
static inline uint16_t htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort >> 8) & 0xFF);
}

// Преобразование network byte order в host byte order
static inline uint16_t ntohs(uint16_t netshort) {
    return ((netshort & 0xFF) << 8) | ((netshort >> 8) & 0xFF);
}

static inline uint32_t htonl(uint32_t hostlong) {
    return ((hostlong & 0xFF) << 24) | ((hostlong & 0xFF00) << 8) |
           ((hostlong & 0xFF0000) >> 8) | ((hostlong & 0xFF000000) >> 24);
}

static inline uint32_t ntohl(uint32_t netlong) {
    return ((netlong & 0xFF) << 24) | ((netlong & 0xFF00) << 8) |
           ((netlong & 0xFF0000) >> 8) | ((netlong & 0xFF000000) >> 24);
}

// Вычислить IP checksum
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

// Создать IP пакет
int ip_create_packet(void* buffer, size_t buffer_size,
                    uint32_t src_ip, uint32_t dest_ip,
                    uint8_t protocol, const void* payload, size_t payload_size) {
    if (!buffer || buffer_size < sizeof(struct ip_header) + payload_size) {
        return -1;
    }
    
    struct ip_header* header = (struct ip_header*)buffer;
    
    // Заполняем заголовок
    header->version_ihl = 0x45; // IPv4, IHL=5 (20 байт заголовок)
    header->tos = 0;
    // total_length в network byte order (big-endian)
    uint16_t total_len = sizeof(struct ip_header) + payload_size;
    header->total_length = htons(total_len);
    header->id = 0; // Упрощенно
    header->flags_fragment = htons(0x4000); // Don't Fragment (big-endian)
    header->ttl = 64;
    header->protocol = protocol;
    header->src_ip = htonl(src_ip);
    header->dest_ip = htonl(dest_ip);
    header->checksum = 0;

    // Копируем payload
    if (payload && payload_size > 0) {
        uint8_t* payload_ptr = (uint8_t*)buffer + sizeof(struct ip_header);
        const uint8_t* src = (const uint8_t*)payload;
        for (size_t i = 0; i < payload_size; i++) {
            payload_ptr[i] = src[i];
        }
    }

    uint16_t csum = ip_checksum(header, sizeof(struct ip_header));
    header->checksum = htons(csum);
    
    return sizeof(struct ip_header) + payload_size;
}

// Парсить IP заголовок
int ip_parse_header(const void* packet, size_t packet_size,
                   struct ip_header* header, const void** payload, size_t* payload_size) {
    if (!packet || packet_size < sizeof(struct ip_header)) {
        return -1;
    }
    
    const struct ip_header* hdr = (const struct ip_header*)packet;
    
    // Проверяем версию
    if ((hdr->version_ihl >> 4) != 4) {
        return -1; // Не IPv4
    }
    
    // Получаем IHL
    uint8_t ihl = (hdr->version_ihl & 0x0F) * 4;
    if (ihl < 20 || packet_size < ihl) {
        return -1; // Некорректный заголовок
    }

    uint16_t total_len = ((hdr->total_length & 0xFF) << 8) | ((hdr->total_length >> 8) & 0xFF);
    if (total_len < ihl || total_len > packet_size) {
        return -1;
    }
    
    uint16_t saved_checksum = hdr->checksum;
    struct ip_header verify = *hdr;
    verify.checksum = 0;
    uint16_t calc_checksum = ip_checksum(&verify, ihl);
    if (calc_checksum != ntohs(saved_checksum)) {
        return -1;
    }

    if (header) {
        *header = *hdr;
        header->src_ip = ntohl(hdr->src_ip);
        header->dest_ip = ntohl(hdr->dest_ip);
    }
    
    if (payload) {
        *payload = (const uint8_t*)packet + ihl;
    }
    
    if (payload_size) {
        *payload_size = total_len - ihl;
    }
    
    return 0;
}

// Получить протокол из IP пакета
uint8_t ip_get_protocol(const void* packet, size_t packet_size) {
    if (!packet || packet_size < sizeof(struct ip_header)) {
        return 0;
    }
    
    const struct ip_header* header = (const struct ip_header*)packet;
    return header->protocol;
}

// Установить наш IP адрес
void ip_set_our_ip(uint32_t ip) {
    our_ip_address = ip;
    arp_set_our_ip(ip);
}

// Получить наш IP адрес
uint32_t ip_get_our_ip() {
    return our_ip_address;
}

// Установить шлюз по умолчанию
void ip_set_gateway(uint32_t gateway) {
    gateway_ip = gateway;
}

// Получить шлюз по умолчанию
uint32_t ip_get_gateway() {
    return gateway_ip;
}

// Установить маску подсети
void ip_set_subnet_mask(uint32_t mask) {
    subnet_mask = mask;
}

// Получить маску подсети
uint32_t ip_get_subnet_mask() {
    return subnet_mask;
}

static void ip_format_octet(uint8_t octet, char* buf, int* pos) {
    if (octet >= 100) buf[(*pos)++] = '0' + (octet / 100);
    if (octet >= 10) buf[(*pos)++] = '0' + ((octet / 10) % 10);
    buf[(*pos)++] = '0' + (octet % 10);
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
            current = current * 10 + (c - '0');
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
    if (ip != 0) {
        ip_set_our_ip(ip);
    }
    if (mask != 0) {
        ip_set_subnet_mask(mask);
    }
    if (gateway != 0) {
        ip_set_gateway(gateway);
    }
    if (dns != 0) {
        dns_set_server(dns);
    }
}

uint32_t ip_resolve_next_hop(uint32_t dest_ip) {
    if (dest_ip == 0xFFFFFFFF) {
        return dest_ip;
    }
    uint32_t our_ip = ip_get_our_ip();
    if (our_ip == 0) {
        return dest_ip;
    }
    uint32_t mask = ip_get_subnet_mask();
    if ((dest_ip & mask) == (our_ip & mask)) {
        return dest_ip;
    }
    uint32_t gateway = ip_get_gateway();
    if (gateway != 0) {
        return gateway;
    }
    return dest_ip;
}

bool ip_is_local_dest(uint32_t dest_ip) {
    if (dest_ip == 0xFFFFFFFF || dest_ip == 0) {
        return true;
    }
    uint32_t our_ip = ip_get_our_ip();
    if (our_ip != 0 && dest_ip == our_ip) {
        return true;
    }
    return dhcp_accepts_dest_ip(dest_ip);
}
