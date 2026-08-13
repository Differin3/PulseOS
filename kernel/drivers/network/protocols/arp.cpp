#include "arp.h"
#include "ethernet.h"
#include "../nic.h"
#include "serial_log.h"
#include "tcp_connection.h"
#include "../core/net_wait.h"
#include "drivers/timer/pit.h"
#include <stddef.h>

static inline uint16_t htons(uint16_t v) {
    return (uint16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF));
}

static inline uint16_t ntohs(uint16_t v) {
    return htons(v);
}

static inline uint32_t htonl(uint32_t hostlong) {
    return ((hostlong & 0xFF) << 24) | ((hostlong & 0xFF00) << 8) |
           ((hostlong & 0xFF0000) >> 8) | ((hostlong & 0xFF000000) >> 24);
}

static inline uint32_t ntohl(uint32_t netlong) {
    return ((netlong & 0xFF) << 24) | ((netlong & 0xFF00) << 8) |
           ((netlong & 0xFF0000) >> 8) | ((netlong & 0xFF000000) >> 24);
}

static struct arp_entry arp_table[ARP_TABLE_SIZE];
static uint32_t our_ip_address = 0;
static uint8_t our_mac_address[6];
static int arp_evict_slot = 0;

static void arp_touch_entry(int idx) {
    arp_table[idx].last_seen = tcp_get_time();
}

// Инициализация ARP
void arp_init() {
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        arp_table[i].valid = false;
        arp_table[i].ip_address = 0;
        arp_table[i].last_seen = 0;
        for (int j = 0; j < 6; j++) {
            arp_table[i].mac_address[j] = 0;
        }
    }
    
    // Получаем наш MAC адрес
    nic_get_mac(our_mac_address);
}

// Обработать входящий ARP пакет
void arp_handle_packet(const struct arp_packet* arp, size_t len) {
    if (!arp || len < sizeof(struct arp_packet)) return;
    
    // Проверяем что это Ethernet + IPv4
    if (ntohs(arp->hardware_type) != 1 || ntohs(arp->protocol_type) != 0x0800) return;
    if (arp->hardware_len != 6 || arp->protocol_len != 4) return;

    uint16_t operation = ntohs(arp->operation);
    arp_add_entry(ntohl(arp->sender_ip), arp->sender_mac);

    if (operation == ARP_OP_REQUEST && ntohl(arp->target_ip) == our_ip_address) {
        struct arp_packet reply;
        reply.hardware_type = htons(1);
        reply.protocol_type = htons(0x0800);
        reply.hardware_len = 6;
        reply.protocol_len = 4;
        reply.operation = htons(ARP_OP_REPLY);
        
        // Наш MAC и IP
        for (int i = 0; i < 6; i++) {
            reply.sender_mac[i] = our_mac_address[i];
        }
        reply.sender_ip = htonl(our_ip_address);

        // MAC и IP запросившего (target_ip остаётся в network byte order)
        for (int i = 0; i < 6; i++) {
            reply.target_mac[i] = arp->sender_mac[i];
        }
        reply.target_ip = arp->sender_ip;
        
        // Создаем Ethernet фрейм
        uint8_t frame[sizeof(struct ethernet_header) + sizeof(struct arp_packet)];
        ethernet_create_frame(frame, sizeof(frame),
                            arp->sender_mac, our_mac_address,
                            ETH_TYPE_ARP, &reply, sizeof(reply));
        
        // Отправляем
        nic_send_packet(frame, sizeof(frame));
    }
}

// Отправить ARP запрос
int arp_send_request(uint32_t target_ip) {
    struct arp_packet request;
    request.hardware_type = htons(1);
    request.protocol_type = htons(0x0800);
    request.hardware_len = 6;
    request.protocol_len = 4;
    request.operation = htons(ARP_OP_REQUEST);
    
    // Наш MAC и IP
    for (int i = 0; i < 6; i++) {
        request.sender_mac[i] = our_mac_address[i];
    }
    request.sender_ip = htonl(our_ip_address);

    // Broadcast MAC для запроса
    for (int i = 0; i < 6; i++) {
        request.target_mac[i] = 0xFF;
    }
    request.target_ip = htonl(target_ip);
    
    // Создаем Ethernet фрейм с broadcast адресом
    uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t frame[sizeof(struct ethernet_header) + sizeof(struct arp_packet)];
    ethernet_create_frame(frame, sizeof(frame),
                        broadcast_mac, our_mac_address,
                        ETH_TYPE_ARP, &request, sizeof(request));
    
    return nic_send_packet(frame, sizeof(frame));
}

// Получить MAC адрес по IP
bool arp_get_mac(uint32_t ip_address, uint8_t* mac_address) {
    if (!mac_address) return false;
    
    // Ищем в таблице
    uint32_t now = tcp_get_time();
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip_address == ip_address) {
            if (now - arp_table[i].last_seen > ARP_ENTRY_TTL_MS) {
                arp_table[i].valid = false;
                return false;
            }
            for (int j = 0; j < 6; j++) {
                mac_address[j] = arp_table[i].mac_address[j];
            }
            return true;
        }
    }
    
    return false; // Не найден
}

// Разрешить MAC адрес (с ожиданием ARP-ответа)
int arp_resolve(uint32_t ip_address, uint8_t* mac_address, int timeout_ms) {
    if (!mac_address) return -1;

    if (ip_address == 0xFFFFFFFF) {
        for (int i = 0; i < 6; i++) {
            mac_address[i] = 0xFF;
        }
        return 0;
    }

    if (arp_get_mac(ip_address, mac_address)) {
        return 0;
    }

    if (arp_send_request(ip_address) != 0) {
        return -1;
    }

    uint32_t start = timer_ms();
    int retry = 0;
    while (timer_ms_since(start) < (uint32_t)(timeout_ms > 0 ? timeout_ms : 3000)) {
        nic_process_packets();
        if (arp_get_mac(ip_address, mac_address)) {
            return 0;
        }
        if ((retry++ % 10) == 9) {
            arp_send_request(ip_address);
        }
        net_wait_ms(10);
    }

    return -1;
}

// Добавить запись в ARP таблицу
void arp_add_entry(uint32_t ip_address, const uint8_t* mac_address) {
    if (!mac_address) return;
    
    // Ищем существующую запись
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip_address == ip_address) {
            for (int j = 0; j < 6; j++) {
                arp_table[i].mac_address[j] = mac_address[j];
            }
            arp_touch_entry(i);
            return;
        }
    }

    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            arp_table[i].ip_address = ip_address;
            for (int j = 0; j < 6; j++) {
                arp_table[i].mac_address[j] = mac_address[j];
            }
            arp_table[i].valid = true;
            arp_touch_entry(i);
            return;
        }
    }

    int slot = arp_evict_slot;
    arp_evict_slot = (arp_evict_slot + 1) % ARP_TABLE_SIZE;
    arp_table[slot].ip_address = ip_address;
    for (int j = 0; j < 6; j++) {
        arp_table[slot].mac_address[j] = mac_address[j];
    }
    arp_table[slot].valid = true;
    arp_touch_entry(slot);
}

// Установить наш IP адрес
void arp_set_our_ip(uint32_t ip) {
    our_ip_address = ip;
}

// Получить наш IP адрес
uint32_t arp_get_our_ip() {
    return our_ip_address;
}

void arp_foreach_entry(arp_entry_fn fn, void* userdata) {
    if (!fn) return;
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid) {
            fn(arp_table[i].ip_address, arp_table[i].mac_address, userdata);
        }
    }
}
