#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ARP типы операций
#define ARP_OP_REQUEST 1  // ARP запрос
#define ARP_OP_REPLY   2  // ARP ответ

// Структура ARP пакета
struct arp_packet {
    uint16_t hardware_type;    // 1 = Ethernet
    uint16_t protocol_type;    // 0x0800 = IPv4
    uint8_t hardware_len;      // 6 для Ethernet
    uint8_t protocol_len;       // 4 для IPv4
    uint16_t operation;        // ARP_OP_*
    uint8_t sender_mac[6];      // MAC отправителя
    uint32_t sender_ip;         // IP отправителя
    uint8_t target_mac[6];     // MAC получателя
    uint32_t target_ip;         // IP получателя
} __attribute__((packed));

#define ARP_TABLE_SIZE 16  // Размер ARP таблицы
#define ARP_ENTRY_TTL_MS 300000

// Запись ARP таблицы
struct arp_entry {
    uint32_t ip_address;       // IP адрес (host byte order)
    uint8_t mac_address[6];    // Соответствующий MAC адрес
    bool valid;                 // Флаг валидности
    uint32_t last_seen;         // Время последнего обновления (ms)
};

// Инициализация ARP
void arp_init();

// Обработать входящий ARP пакет
void arp_handle_packet(const struct arp_packet* arp, size_t len);

// Отправить ARP запрос для получения MAC адреса
int arp_send_request(uint32_t target_ip);

// Получить MAC адрес по IP (из таблицы)
bool arp_get_mac(uint32_t ip_address, uint8_t* mac_address);

// Разрешить MAC адрес (с ожиданием ARP-ответа)
int arp_resolve(uint32_t ip_address, uint8_t* mac_address, int timeout_ms);

// Добавить запись в ARP таблицу
void arp_add_entry(uint32_t ip_address, const uint8_t* mac_address);

// Установить наш IP адрес
void arp_set_our_ip(uint32_t ip);

// Получить наш IP адрес
uint32_t arp_get_our_ip();

// Обход валидных записей ARP таблицы
typedef void (*arp_entry_fn)(uint32_t ip, const uint8_t* mac, void* userdata);
void arp_foreach_entry(arp_entry_fn fn, void* userdata);

#endif
