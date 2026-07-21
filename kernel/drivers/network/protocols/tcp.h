#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// TCP флаги
#define TCP_FLAG_FIN  0x01
#define TCP_FLAG_SYN  0x02
#define TCP_FLAG_RST  0x04
#define TCP_FLAG_PSH  0x08
#define TCP_FLAG_ACK  0x10
#define TCP_FLAG_URG  0x20

// TCP заголовок (20+ байт)
struct tcp_header {
    uint16_t src_port;      // Порт отправителя
    uint16_t dest_port;     // Порт получателя
    uint32_t seq_num;       // Sequence number
    uint32_t ack_num;       // Acknowledgment number
    uint8_t  data_offset;   // 4 бита offset + 4 бита зарезервировано
    uint8_t  flags;         // TCP флаги
    uint16_t window;        // Window size
    uint16_t checksum;      // Checksum
    uint16_t urgent_ptr;     // Urgent pointer
} __attribute__((packed));

// Состояния TCP соединения
enum tcp_state {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_TIME_WAIT,
    TCP_LAST_ACK
};

// Максимальный размер TCP payload (без заголовка)
#define TCP_MAX_PAYLOAD 1460  // 1500 - 20 IP - 20 TCP

// Создать TCP сегмент
int tcp_create_packet(void* buffer, size_t buffer_size,
                     uint16_t src_port, uint16_t dest_port,
                     uint32_t seq_num, uint32_t ack_num,
                     uint8_t flags, uint16_t window,
                     const void* payload, size_t payload_size);

// Парсить TCP заголовок
int tcp_parse_header(const void* packet, size_t packet_size,
                    struct tcp_header* header, const void** payload, size_t* payload_size);

// Вычислить TCP checksum (pseudo-header + TCP header + data)
uint16_t tcp_checksum(uint32_t src_ip, uint32_t dest_ip,
                     const struct tcp_header* header, size_t tcp_len);

// Отправить TCP сегмент через IP
int tcp_send(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port,
            uint32_t seq_num, uint32_t ack_num,
            uint8_t flags, uint16_t window,
            const void* payload, size_t payload_size);

// Обработать входящий TCP пакет
void tcp_handle_packet(uint32_t src_ip, uint32_t dest_ip,
                      const void* ip_payload, size_t payload_size);

// TCP API для приложений
// Создать TCP сокет и подключиться
struct tcp_connection* tcp_connect(uint32_t dest_ip, uint16_t dest_port);

// Дождаться установления TCP соединения
int tcp_connect_wait(struct tcp_connection* conn, int timeout_ms);

// Прослушивать порт
struct tcp_connection* tcp_listen(uint16_t port);

// Дождаться входящего соединения на LISTEN-сокете
struct tcp_connection* tcp_accept(struct tcp_connection* listen_conn, int timeout_ms);

// Отправить данные через TCP соединение
int tcp_send_data(struct tcp_connection* conn, const void* data, size_t len);

// Получить данные из TCP соединения
int tcp_recv_data(struct tcp_connection* conn, void* buffer, size_t max_len);

// Закрыть TCP соединение
void tcp_close(struct tcp_connection* conn);

// Инициализация TCP стека
void tcp_init();

#endif
