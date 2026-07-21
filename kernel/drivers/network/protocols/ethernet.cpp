#include "ethernet.h"
#include <stddef.h>

// Преобразование host byte order в network byte order (big-endian)
static inline uint16_t htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort >> 8) & 0xFF);
}

// Преобразование network byte order в host byte order
static inline uint16_t ntohs(uint16_t netshort) {
    return ((netshort & 0xFF) << 8) | ((netshort >> 8) & 0xFF);
}

// Создать Ethernet фрейм
int ethernet_create_frame(void* buffer, size_t buffer_size,
                         const uint8_t* dest_mac, const uint8_t* src_mac,
                         uint16_t protocol_type, const void* payload, size_t payload_size) {
    if (!buffer || buffer_size < sizeof(struct ethernet_header) + payload_size) {
        return -1; // Буфер слишком мал
    }
    
    struct ethernet_header* header = (struct ethernet_header*)buffer;
    
    // Копируем MAC адреса
    if (dest_mac) {
        for (int i = 0; i < 6; i++) {
            header->dest_mac[i] = dest_mac[i];
        }
    } else {
        // Broadcast адрес
        for (int i = 0; i < 6; i++) {
            header->dest_mac[i] = 0xFF;
        }
    }
    
    if (src_mac) {
        for (int i = 0; i < 6; i++) {
            header->src_mac[i] = src_mac[i];
        }
    } else {
        // Нулевой адрес
        for (int i = 0; i < 6; i++) {
            header->src_mac[i] = 0;
        }
    }
    
    // Устанавливаем тип протокола (network byte order - big-endian)
    header->type = htons(protocol_type);
    
    // Копируем payload
    if (payload && payload_size > 0) {
        uint8_t* payload_ptr = (uint8_t*)buffer + sizeof(struct ethernet_header);
        const uint8_t* src = (const uint8_t*)payload;
        for (size_t i = 0; i < payload_size; i++) {
            payload_ptr[i] = src[i];
        }
    }
    
    return sizeof(struct ethernet_header) + payload_size;
}

// Парсить Ethernet заголовок
int ethernet_parse_header(const void* frame, size_t frame_size,
                         struct ethernet_header* header, const void** payload, size_t* payload_size) {
    if (!frame || frame_size < sizeof(struct ethernet_header)) {
        return -1; // Фрейм слишком мал
    }
    
    const struct ethernet_header* hdr = (const struct ethernet_header*)frame;
    
    if (header) {
        *header = *hdr;
    }
    
    if (payload) {
        *payload = (const uint8_t*)frame + sizeof(struct ethernet_header);
    }
    
    if (payload_size) {
        *payload_size = frame_size - sizeof(struct ethernet_header);
    }
    
    return 0;
}

// Получить тип протокола из фрейма
uint16_t ethernet_get_protocol(const void* frame, size_t frame_size) {
    if (!frame || frame_size < sizeof(struct ethernet_header)) {
        return 0;
    }
    
    const struct ethernet_header* header = (const struct ethernet_header*)frame;
    return ntohs(header->type); // Конвертируем из network byte order
}
