#include "dns.h"
#include "../protocols/udp.h"
#include "../protocols/ip.h"
#include "../protocols/tcp_connection.h"  // Для tcp_get_time()
#include "../nic.h"
#include "serial_log.h"
#include <stddef.h>

// Вспомогательные функции для byte order (объявляем в начале)
static inline uint16_t htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort >> 8) & 0xFF);
}

static inline uint32_t htonl(uint32_t hostlong) {
    return ((hostlong & 0xFF) << 24) | ((hostlong & 0xFF00) << 8) |
           ((hostlong & 0xFF0000) >> 8) | ((hostlong & 0xFF000000) >> 24);
}

static inline uint16_t ntohs(uint16_t netshort) {
    return ((netshort & 0xFF) << 8) | ((netshort >> 8) & 0xFF);
}

static struct dns_cache_entry dns_cache[DNS_CACHE_SIZE];
static uint32_t dns_server_ip = 0;
static uint16_t dns_query_id = 1;

static struct {
    bool active;
    uint16_t query_id;
    char hostname[64];
    uint32_t result_ip;
    bool got_response;
} dns_pending = { false, 0, {0}, 0, false };

int dns_parse_response(const void* data, size_t data_size, uint32_t* ip_address);
void dns_add_record(const char* hostname, uint32_t ip_address);

// Установить DNS сервер
void dns_set_server(uint32_t dns_ip) {
    dns_server_ip = dns_ip;
}

// Получить DNS сервер
uint32_t dns_get_server() {
    return dns_server_ip;
}

// Инициализация DNS
void dns_init() {
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        dns_cache[i].valid = false;
        dns_cache[i].hostname[0] = 0;
    }
}

// Кодировать доменное имя в QNAME формат
static int dns_encode_name(const char* hostname, uint8_t* buffer, size_t buffer_size) {
    if (!hostname || !buffer) return -1;
    
    size_t pos = 0;
    const char* start = hostname;
    
    while (*start && pos < buffer_size - 1) {
        const char* dot = start;
        while (*dot && *dot != '.') dot++;
        
        size_t label_len = dot - start;
        if (label_len > 63 || pos + label_len + 1 >= buffer_size) {
            return -1;
        }
        
        buffer[pos++] = (uint8_t)label_len;
        for (size_t i = 0; i < label_len; i++) {
            buffer[pos++] = start[i];
        }
        
        if (*dot == '.') {
            start = dot + 1;
        } else {
            break;
        }
    }
    
    buffer[pos++] = 0;  // Завершающий ноль
    return pos;
}

// Декодировать QNAME в доменное имя
static int dns_decode_name(const uint8_t* buffer, size_t buffer_size, size_t* offset, char* name, size_t name_size) {
    if (!buffer || !offset || !name) return -1;
    
    size_t pos = *offset;
    size_t name_pos = 0;
    bool first = true;
    
    while (pos < buffer_size && name_pos < name_size - 1) {
        uint8_t len = buffer[pos++];
        
        if (len == 0) {
            break;  // Конец имени
        }
        
        // Compression pointer (первые 2 бита = 11)
        if ((len & 0xC0) == 0xC0) {
            if (pos >= buffer_size) return -1;
            uint16_t ptr = ((len & 0x3F) << 8) | buffer[pos++];
            // Рекурсивный вызов для разыменования указателя
            size_t old_pos = pos;
            pos = ptr;
            if (dns_decode_name(buffer, buffer_size, &pos, name + name_pos, name_size - name_pos) < 0) {
                return -1;
            }
            pos = old_pos;
            break;
        }
        
        if (len > 63 || pos + len > buffer_size) {
            return -1;
        }
        
        if (!first && name_pos < name_size - 1) {
            name[name_pos++] = '.';
        }
        first = false;
        
        for (uint8_t i = 0; i < len && name_pos < name_size - 1; i++) {
            name[name_pos++] = buffer[pos++];
        }
    }
    
    name[name_pos] = 0;
    *offset = pos;
    return name_pos;
}

static char dns_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

bool dns_hostname_equal(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (dns_tolower(*a) != dns_tolower(*b)) return false;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static bool dns_cache_match(const char* hostname, const char* cached) {
    return dns_hostname_equal(hostname, cached);
}

int dns_query(uint32_t dns_server, const char* hostname, uint32_t* ip_address) {
    if (!hostname || !ip_address || dns_server == 0) return -1;

    uint32_t current_time = tcp_get_time();
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid) {
            if (current_time - dns_cache[i].timestamp < dns_cache[i].ttl * 1000) {
                if (dns_cache_match(hostname, dns_cache[i].hostname)) {
                    *ip_address = dns_cache[i].ip_address;
                    return 0;
                }
            } else {
                dns_cache[i].valid = false;
            }
        }
    }

    uint8_t dns_buffer[512];
    size_t pos = 0;

    struct dns_header* header = (struct dns_header*)dns_buffer;
    uint16_t this_query_id = dns_query_id++;
    header->id = htons(this_query_id);
    header->flags = htons(DNS_FLAG_RD);
    header->qdcount = htons(1);
    header->ancount = 0;
    header->nscount = 0;
    header->arcount = 0;
    pos = sizeof(struct dns_header);

    int name_len = dns_encode_name(hostname, dns_buffer + pos, sizeof(dns_buffer) - pos);
    if (name_len < 0) return -1;
    pos += (size_t)name_len;

    struct dns_question* question = (struct dns_question*)(dns_buffer + pos);
    question->qtype = htons(DNS_TYPE_A);
    question->qclass = htons(DNS_CLASS_IN);
    pos += sizeof(struct dns_question);

    dns_pending.active = true;
    dns_pending.query_id = this_query_id;
    dns_pending.got_response = false;
    dns_pending.result_ip = 0;
    int hi = 0;
    while (hostname[hi] && hi < 63) {
        dns_pending.hostname[hi] = hostname[hi];
        hi++;
    }
    dns_pending.hostname[hi] = 0;

    if (udp_send(dns_server, DNS_CLIENT_PORT, DNS_PORT, dns_buffer, pos) != 0) {
        dns_pending.active = false;
        log_msg(LOG_ERR, "dns", "udp send failed");
        return -1;
    }

    int attempts = 0;
    const int max_attempts = 200;
    while (attempts < max_attempts) {
        nic_process_packets();

        if (dns_pending.got_response && dns_pending.query_id == this_query_id) {
            *ip_address = dns_pending.result_ip;
            dns_pending.active = false;
            dns_add_record(hostname, dns_pending.result_ip);
            log_ip(LOG_INFO, "dns", "resolved", dns_pending.result_ip);
            return 0;
        }

        if (attempts > 0 && (attempts % 40) == 0) {
            udp_send(dns_server, DNS_CLIENT_PORT, DNS_PORT, dns_buffer, pos);
        }

        for (volatile int i = 0; i < 100000; i++);
        attempts++;
    }

    dns_pending.active = false;
    log_msg(LOG_ERR, "dns", "query timeout");
    return -1;
}

int dns_resolve(const char* hostname, uint32_t* ip_address) {
    if (!hostname || !ip_address) return -1;
    if (dns_server_ip == 0) {
        log_msg(LOG_ERR, "dns", "no server configured");
        return -1;
    }
    return dns_query(dns_server_ip, hostname, ip_address);
}

int net_resolve_host(const char* str, size_t len, uint32_t* ip_address) {
    if (!str || !ip_address) return -1;
    if (ip_parse_address(str, len, ip_address) == 0) return 0;

    char hostname[256];
    size_t n = len;
    if (n >= sizeof(hostname)) n = sizeof(hostname) - 1;
    for (size_t i = 0; i < n; i++) hostname[i] = str[i];
    hostname[n] = 0;

    while (n > 0 && (hostname[n - 1] == ' ' || hostname[n - 1] == '\t')) {
        hostname[--n] = 0;
    }
    if (n == 0) return -1;
    return dns_resolve(hostname, ip_address);
}

// Парсить DNS ответ
int dns_parse_response(const void* data, size_t data_size, uint32_t* ip_address) {
    if (!data || data_size < sizeof(struct dns_header)) return -1;
    
    const struct dns_header* header = (const struct dns_header*)data;
    uint16_t flags = ntohs(header->flags);
    
    // Проверяем что это ответ
    if (!(flags & DNS_FLAG_QR)) return -1;
    
    // Проверяем код ответа (нижние 4 бита)
    if ((flags & DNS_FLAG_RCODE) != 0) return -1;
    
    uint16_t qdcount = ntohs(header->qdcount);
    uint16_t ancount = ntohs(header->ancount);
    
    if (ancount == 0) return -1;  // Нет ответов
    
    size_t pos = sizeof(struct dns_header);
    
    // Пропускаем вопросы
    for (uint16_t i = 0; i < qdcount; i++) {
        char name[256];
        if (dns_decode_name((const uint8_t*)data, data_size, &pos, name, sizeof(name)) < 0) {
            return -1;
        }
        pos += sizeof(struct dns_question);
    }
    
    // Читаем ответы
    for (uint16_t i = 0; i < ancount; i++) {
        char name[256];
        if (dns_decode_name((const uint8_t*)data, data_size, &pos, name, sizeof(name)) < 0) {
            return -1;
        }
        
        if (pos + sizeof(struct dns_rr) > data_size) return -1;
        
        const struct dns_rr* rr = (const struct dns_rr*)((const uint8_t*)data + pos);
        pos += sizeof(struct dns_rr);
        
        uint16_t type = ntohs(rr->type);
        uint16_t rdlength = ntohs(rr->rdlength);
        
        if (type == DNS_TYPE_CNAME) {
            pos += rdlength;
            continue;
        }

        if (type == DNS_TYPE_A && rdlength == 4) {
            // A-запись (IPv4)
            if (pos + 4 > data_size) return -1;
            const uint8_t* ip_bytes = (const uint8_t*)data + pos;
            *ip_address = (ip_bytes[0] << 24) | (ip_bytes[1] << 16) |
                         (ip_bytes[2] << 8) | ip_bytes[3];
            return 0;
        }
        
        pos += rdlength;
    }
    
    return -1;
}

// Обработать входящий DNS ответ (клиент)
void dns_handle_response(const void* udp_payload, size_t payload_size) {
    if (!udp_payload || payload_size < sizeof(struct dns_header)) return;
    if (!dns_pending.active) return;
    
    const struct dns_header* header = (const struct dns_header*)udp_payload;
    uint16_t id = ntohs(header->id);
    if (id != dns_pending.query_id) return;
    
    uint32_t ip_address = 0;
    if (dns_parse_response(udp_payload, payload_size, &ip_address) == 0) {
        dns_pending.result_ip = ip_address;
        dns_pending.got_response = true;
    }
}

// Обработать входящий DNS запрос (сервер)
void dns_handle_request(uint32_t src_ip, uint16_t src_port,
                       const void* udp_payload, size_t payload_size) {
    if (!udp_payload || payload_size < sizeof(struct dns_header)) return;
    
    const struct dns_header* req_header = (const struct dns_header*)udp_payload;
    uint16_t id = ntohs(req_header->id);
    uint16_t qdcount = ntohs(req_header->qdcount);
    
    if (qdcount == 0) return;
    
    // Парсим вопрос
    size_t pos = sizeof(struct dns_header);
    char hostname[256];
    if (dns_decode_name((const uint8_t*)udp_payload, payload_size, &pos, hostname, sizeof(hostname)) < 0) {
        return;
    }
    
    if (pos + sizeof(struct dns_question) > payload_size) return;
    const struct dns_question* question = (const struct dns_question*)((const uint8_t*)udp_payload + pos);
    (void)ntohs(question->qtype);  // Для будущего использования
    
    // Ищем в кэше/базе
    uint32_t ip_address = 0;
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid && dns_cache_match(hostname, dns_cache[i].hostname)) {
                ip_address = dns_cache[i].ip_address;
                break;
        }
    }
    
    // Формируем ответ
    uint8_t response[512];
    struct dns_header* resp_header = (struct dns_header*)response;
    resp_header->id = htons(id);
    resp_header->flags = htons(DNS_FLAG_QR | DNS_FLAG_AA | DNS_FLAG_RD);
    resp_header->qdcount = htons(1);
    resp_header->ancount = ip_address ? htons(1) : 0;
    resp_header->nscount = 0;
    resp_header->arcount = 0;
    
    pos = sizeof(struct dns_header);
    
    // Копируем вопрос
    int name_len = dns_encode_name(hostname, response + pos, sizeof(response) - pos);
    if (name_len < 0) return;
    pos += name_len;
    
    struct dns_question* resp_question = (struct dns_question*)(response + pos);
    resp_question->qtype = question->qtype;
    resp_question->qclass = question->qclass;
    pos += sizeof(struct dns_question);
    
    // Добавляем ответ (если найден)
    if (ip_address) {
        name_len = dns_encode_name(hostname, response + pos, sizeof(response) - pos);
        if (name_len < 0) return;
        pos += name_len;
        
        struct dns_rr* rr = (struct dns_rr*)(response + pos);
        rr->type = htons(DNS_TYPE_A);
        rr->rr_class = htons(DNS_CLASS_IN);
        rr->ttl = htonl(3600);  // 1 час
        rr->rdlength = htons(4);
        pos += sizeof(struct dns_rr);
        
        // IP адрес
        response[pos++] = (ip_address >> 24) & 0xFF;
        response[pos++] = (ip_address >> 16) & 0xFF;
        response[pos++] = (ip_address >> 8) & 0xFF;
        response[pos++] = ip_address & 0xFF;
    }
    
    // Отправляем ответ через UDP
    udp_send(src_ip, DNS_PORT, src_port, response, pos);
}

// Добавить запись в DNS сервер
void dns_add_record(const char* hostname, uint32_t ip_address) {
    if (!hostname) return;
    
    // Ищем свободную ячейку или обновляем существующую
    int idx = -1;
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!dns_cache[i].valid) {
            if (idx < 0) idx = i;
        } else if (dns_cache[i].valid && dns_cache_match(hostname, dns_cache[i].hostname)) {
                idx = i;
                break;
        }
    }
    
    if (idx < 0) idx = 0;  // Перезаписываем первую ячейку
    
    // Копируем hostname
    int i = 0;
    while (hostname[i] && i < 63) {
        dns_cache[idx].hostname[i] = hostname[i];
        i++;
    }
    dns_cache[idx].hostname[i] = 0;
    
    dns_cache[idx].ip_address = ip_address;
    dns_cache[idx].ttl = 3600;
    dns_cache[idx].timestamp = tcp_get_time();
    dns_cache[idx].valid = true;
}
