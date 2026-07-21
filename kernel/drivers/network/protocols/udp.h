#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include <stddef.h>

// Структура UDP заголовка
struct udp_header {
    uint16_t src_port;    // Порт отправителя
    uint16_t dest_port;   // Порт получателя
    uint16_t length;      // Длина (заголовок + данные)
    uint16_t checksum;    // Checksum (опционально, может быть 0)
} __attribute__((packed));

#define UDP_MAX_PAYLOAD 1472  // Максимальный размер payload (1500 - 20 IP - 8 UDP)

// Создать UDP пакет
int udp_create_packet(void* buffer, size_t buffer_size,
                     uint16_t src_port, uint16_t dest_port,
                     const void* payload, size_t payload_size);

// Парсить UDP заголовок
int udp_parse_header(const void* packet, size_t packet_size,
                    struct udp_header* header, const void** payload, size_t* payload_size);

// Вычислить UDP checksum (pseudo-header + UDP header + data)
uint16_t udp_checksum(uint32_t src_ip, uint32_t dest_ip,
                      const struct udp_header* header, size_t udp_len);

// Отправить UDP пакет через IP
int udp_send(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port,
             const void* payload, size_t payload_size);

// Обработать входящий UDP пакет
void udp_handle_packet(uint32_t src_ip, uint32_t dest_ip,
                      const void* ip_payload, size_t payload_size);

// Зарегистрировать порт для UDP echo (udplisten)
int udp_listen_port(uint16_t port);

// Снять регистрацию UDP echo-порта
void udp_unlisten_port(uint16_t port);

#endif
