#ifndef DNS_H
#define DNS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// DNS порт
#define DNS_PORT 53
#define DNS_CLIENT_PORT 49152

// DNS типы записей
#define DNS_TYPE_A     1   // IPv4 адрес
#define DNS_TYPE_NS    2   // Name Server
#define DNS_TYPE_CNAME 5   // Canonical Name
#define DNS_TYPE_AAAA  28  // IPv6 адрес

// DNS классы
#define DNS_CLASS_IN   1   // Internet

// DNS заголовок
struct dns_header {
    uint16_t id;        // Transaction ID
    uint16_t flags;     // Flags
    uint16_t qdcount;   // Количество вопросов
    uint16_t ancount;   // Количество ответов
    uint16_t nscount;   // Количество authority records
    uint16_t arcount;   // Количество additional records
} __attribute__((packed));

// DNS флаги
#define DNS_FLAG_QR    0x8000  // Query/Response (1 = Response)
#define DNS_FLAG_OPCODE 0x7800 // Opcode
#define DNS_FLAG_AA    0x0400  // Authoritative Answer
#define DNS_FLAG_TC    0x0200  // Truncated
#define DNS_FLAG_RD    0x0100  // Recursion Desired
#define DNS_FLAG_RA    0x0080  // Recursion Available
#define DNS_FLAG_RCODE 0x000F // Response Code

// DNS вопрос
struct dns_question {
    // QNAME (variable length, заканчивается 0)
    uint16_t qtype;     // Тип записи
    uint16_t qclass;    // Класс
} __attribute__((packed));

// DNS Resource Record
struct dns_rr {
    // NAME (variable length)
    uint16_t type;       // Тип записи
    uint16_t rr_class;   // Класс (class - зарезервированное слово в C++)
    uint32_t ttl;        // Time To Live
    uint16_t rdlength;   // Длина данных
    // RDATA (variable length)
} __attribute__((packed));

#define DNS_CACHE_SIZE 16

// DNS кэш запись
struct dns_cache_entry {
    char hostname[64];
    uint32_t ip_address;
    uint32_t ttl;
    uint32_t timestamp;
    bool valid;
};

// Инициализация DNS
void dns_init();

// Отправить DNS запрос
int dns_query(uint32_t dns_server_ip, const char* hostname, uint32_t* ip_address);

// Разрешить доменное имя через настроенный DNS сервер
int dns_resolve(const char* hostname, uint32_t* ip_address);

// Разрешить IP или доменное имя (для ping, route и т.д.)
int net_resolve_host(const char* str, size_t len, uint32_t* ip_address);

// Сравнение hostname без учёта регистра
bool dns_hostname_equal(const char* a, const char* b);

// Обработать входящий DNS запрос (сервер)
void dns_handle_request(uint32_t src_ip, uint16_t src_port,
                       const void* udp_payload, size_t payload_size);

// Обработать входящий DNS ответ (клиент)
void dns_handle_response(const void* udp_payload, size_t payload_size);

// Добавить запись в DNS сервер
void dns_add_record(const char* hostname, uint32_t ip_address);

// Установить DNS сервер
void dns_set_server(uint32_t dns_ip);

// Получить DNS сервер
uint32_t dns_get_server();

#endif
