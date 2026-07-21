#ifndef IP_H
#define IP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// IP протоколы
#define IP_PROTOCOL_ICMP 1
#define IP_PROTOCOL_TCP  6
#define IP_PROTOCOL_UDP  17

// Структура IP заголовка
struct ip_header {
    uint8_t version_ihl;      // Версия (4 бита) + IHL (4 бита)
    uint8_t tos;              // Type of Service
    uint16_t total_length;    // Общая длина пакета
    uint16_t id;              // Identification
    uint16_t flags_fragment;  // Flags (3 бита) + Fragment Offset (13 бит)
    uint8_t ttl;              // Time To Live
    uint8_t protocol;         // Протокол (IP_PROTOCOL_*)
    uint16_t checksum;         // Checksum
    uint32_t src_ip;          // IP адрес отправителя
    uint32_t dest_ip;         // IP адрес получателя
} __attribute__((packed));

// Создать IP пакет
int ip_create_packet(void* buffer, size_t buffer_size,
                    uint32_t src_ip, uint32_t dest_ip,
                    uint8_t protocol, const void* payload, size_t payload_size);

// Парсить IP заголовок
int ip_parse_header(const void* packet, size_t packet_size,
                   struct ip_header* header, const void** payload, size_t* payload_size);

// Получить протокол из IP пакета
uint8_t ip_get_protocol(const void* packet, size_t packet_size);

// Вычислить IP checksum
uint16_t ip_checksum(const void* data, size_t len);

// Установить наш IP адрес
void ip_set_our_ip(uint32_t ip);

// Получить наш IP адрес
uint32_t ip_get_our_ip();

// Установить шлюз по умолчанию
void ip_set_gateway(uint32_t gateway);

// Получить шлюз по умолчанию
uint32_t ip_get_gateway();

// Установить маску подсети
void ip_set_subnet_mask(uint32_t mask);

// Получить маску подсети
uint32_t ip_get_subnet_mask();

// Форматировать IP в строку (a.b.c.d)
void ip_format_address(uint32_t ip, char* buf, size_t buflen);

// Разобрать IP из строки (поддерживает ведущие нули)
int ip_parse_address(const char* str, size_t len, uint32_t* out);

// IP для ARP-запроса (шлюз, если адрес вне подсети)
uint32_t ip_resolve_next_hop(uint32_t dest_ip);

// Принять ли входящий пакет с данным dest IP
bool ip_is_local_dest(uint32_t dest_ip);

// Разобрать следующий IP-токен из строки (пробелы как разделители)
int ip_parse_address_token(const char* str, size_t len, size_t* pos, uint32_t* out);

// Применить сетевую конфигурацию (0 = не менять поле)
void network_apply_config(uint32_t ip, uint32_t mask, uint32_t gateway, uint32_t dns);

#endif
