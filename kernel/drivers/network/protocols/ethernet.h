#ifndef ETHERNET_H
#define ETHERNET_H

#include <stdint.h>
#include <stddef.h>

// Ethernet типы протоколов
#define ETH_TYPE_IPV4   0x0800  // IPv4
#define ETH_TYPE_ARP    0x0806  // ARP
#define ETH_TYPE_IPV6   0x86DD  // IPv6

// Структура Ethernet заголовка
struct ethernet_header {
    uint8_t dest_mac[6];    // MAC адрес получателя
    uint8_t src_mac[6];     // MAC адрес отправителя
    uint16_t type;          // Тип протокола (ETH_TYPE_*)
} __attribute__((packed));

// Создать Ethernet фрейм
int ethernet_create_frame(void* buffer, size_t buffer_size,
                         const uint8_t* dest_mac, const uint8_t* src_mac,
                         uint16_t protocol_type, const void* payload, size_t payload_size);

// Парсить Ethernet заголовок
int ethernet_parse_header(const void* frame, size_t frame_size,
                         struct ethernet_header* header, const void** payload, size_t* payload_size);

// Получить тип протокола из фрейма
uint16_t ethernet_get_protocol(const void* frame, size_t frame_size);

#endif
