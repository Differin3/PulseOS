#ifndef TCP_CONNECTION_H
#define TCP_CONNECTION_H

#include "tcp.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TCP_MAX_CONNECTIONS 32
#define TCP_RX_BUFFER_SIZE 4096
#define TCP_TX_BUFFER_SIZE 4096
#define TCP_MAX_RETRANSMIT 5
#define TCP_RTO_INITIAL 1000  // 1 секунда в миллисекундах
#define TCP_RTO_MAX 60000     // 60 секунд
#define TCP_MAX_PAYLOAD 1460  // Максимальный размер TCP payload

// TCP соединение
struct tcp_connection {
    uint32_t src_ip;
    uint16_t src_port;
    uint32_t dest_ip;
    uint16_t dest_port;
    
    enum tcp_state state;
    
    // Sequence numbers
    uint32_t send_seq;      // Следующий sequence number для отправки
    uint32_t send_una;      // Unacknowledged sequence number
    uint32_t recv_seq;       // Следующий ожидаемый sequence number
    
    // Windows
    uint16_t send_window;   // Размер окна отправки
    uint16_t recv_window;   // Размер окна приема
    uint16_t mss;           // Maximum Segment Size
    
    // Buffers
    uint8_t rx_buffer[TCP_RX_BUFFER_SIZE];
    size_t rx_buffer_len;
    uint8_t tx_buffer[TCP_TX_BUFFER_SIZE];
    size_t tx_buffer_len;
    
    // Retransmission
    struct {
        uint32_t seq;
        size_t len;
        uint32_t timeout;
        uint32_t rto;
        int retries;
        uint8_t data[TCP_MAX_PAYLOAD];  // Данные для retransmission
    } retransmit_queue[TCP_MAX_RETRANSMIT];
    int retransmit_count;
    
    // Congestion control
    uint32_t cwnd;          // Congestion window
    uint32_t ssthresh;      // Slow start threshold
    bool in_slow_start;
    
    // Timers
    uint32_t last_activity;

    bool pending_accept;
    bool valid;
};

// Инициализация TCP connection manager
void tcp_connection_init();

// Создать новое TCP соединение
struct tcp_connection* tcp_connection_create(uint32_t src_ip, uint16_t src_port,
                                             uint32_t dest_ip, uint16_t dest_port);

// Найти TCP соединение
struct tcp_connection* tcp_connection_find(uint32_t src_ip, uint16_t src_port,
                                            uint32_t dest_ip, uint16_t dest_port);

// Найти соединение в состоянии LISTEN по порту
struct tcp_connection* tcp_connection_find_listen(uint16_t port);

// Принять установленное входящее соединение на локальном порту
struct tcp_connection* tcp_connection_accept_pending(uint16_t local_port,
                                                     struct tcp_connection* listen_conn);

// Выделить слот LISTEN без client tuple
struct tcp_connection* tcp_connection_alloc_listen(uint32_t src_ip, uint16_t port);

// Отправить данные через TCP соединение
int tcp_connection_send(struct tcp_connection* conn, const void* data, size_t len);

// Получить данные из TCP соединения
int tcp_connection_receive(struct tcp_connection* conn, void* buffer, size_t max_len);

// Закрыть TCP соединение
void tcp_connection_close(struct tcp_connection* conn);

// Обработать таймеры (retransmission, timeouts)
void tcp_process_timers();

// Получить текущее время (milliseconds, упрощенно)
uint32_t tcp_get_time();

// Увеличить счетчик времени (вызывать периодически)
void tcp_increment_time();

// Добавить сегмент в очередь retransmission
void tcp_add_retransmit(struct tcp_connection* conn, uint32_t seq, size_t len, const void* data);

// Удалить подтвержденные сегменты из очереди retransmission
void tcp_ack_retransmit(struct tcp_connection* conn, uint32_t ack_num);

// Обход активных TCP соединений
typedef void (*tcp_conn_fn)(struct tcp_connection* conn, void* userdata);
void tcp_foreach_connection(tcp_conn_fn fn, void* userdata);

#endif
