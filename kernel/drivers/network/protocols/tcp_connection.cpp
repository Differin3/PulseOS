#include "tcp_connection.h"
#include "tcp.h"
#include "drivers/timer/pit.h"
#include <stddef.h>

static struct tcp_connection tcp_connections[TCP_MAX_CONNECTIONS];

// Глобальная переменная для ISN (используется в tcp.cpp)
uint32_t tcp_next_isn = 1000;  // Initial Sequence Number

// Инициализация
void tcp_connection_init() {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_connections[i].valid = false;
        tcp_connections[i].state = TCP_CLOSED;
        tcp_connections[i].rx_buffer_len = 0;
        tcp_connections[i].tx_buffer_len = 0;
        tcp_connections[i].retransmit_count = 0;
    }
}

// Время в миллисекундах (PIT jiffies)
uint32_t tcp_get_time() {
    return timer_ms();
}

void tcp_increment_time() {
    // retained for API compatibility; PIT advances time
}

// Создать новое TCP соединение
struct tcp_connection* tcp_connection_create(uint32_t src_ip, uint16_t src_port,
                                             uint32_t dest_ip, uint16_t dest_port) {
    // Ищем свободную ячейку
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!tcp_connections[i].valid) {
            struct tcp_connection* conn = &tcp_connections[i];
            
            conn->src_ip = src_ip;
            conn->src_port = src_port;
            conn->dest_ip = dest_ip;
            conn->dest_port = dest_port;
            conn->state = TCP_SYN_SENT;
            conn->send_seq = tcp_next_isn++;
            conn->send_una = conn->send_seq;
            conn->recv_seq = 0;
            conn->send_window = TCP_RX_BUFFER_SIZE;
            conn->recv_window = TCP_RX_BUFFER_SIZE;
            conn->mss = 1460;
            conn->rx_buffer_len = 0;
            conn->tx_buffer_len = 0;
            conn->retransmit_count = 0;
            conn->cwnd = 1;  // Начинаем с 1 сегмента
            conn->ssthresh = 65535;
            conn->in_slow_start = true;
            conn->last_activity = tcp_get_time();
            conn->pending_accept = false;
            conn->valid = true;
            
            return conn;
        }
    }
    return NULL;  // Нет свободных соединений
}

// Найти TCP соединение
struct tcp_connection* tcp_connection_find(uint32_t src_ip, uint16_t src_port,
                                          uint32_t dest_ip, uint16_t dest_port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_connections[i].valid &&
            tcp_connections[i].src_ip == src_ip &&
            tcp_connections[i].src_port == src_port &&
            tcp_connections[i].dest_ip == dest_ip &&
            tcp_connections[i].dest_port == dest_port) {
            return &tcp_connections[i];
        }
    }
    return NULL;
}

// Найти соединение в состоянии LISTEN
struct tcp_connection* tcp_connection_find_listen(uint16_t port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_connections[i].valid &&
            tcp_connections[i].state == TCP_LISTEN &&
            tcp_connections[i].src_port == port) {
            return &tcp_connections[i];
        }
    }
    return NULL;
}

struct tcp_connection* tcp_connection_alloc_listen(uint32_t src_ip, uint16_t port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!tcp_connections[i].valid) {
            struct tcp_connection* conn = &tcp_connections[i];
            conn->src_ip = src_ip;
            conn->src_port = port;
            conn->dest_ip = 0;
            conn->dest_port = 0;
            conn->state = TCP_LISTEN;
            conn->send_seq = 0;
            conn->send_una = 0;
            conn->recv_seq = 0;
            conn->send_window = TCP_RX_BUFFER_SIZE;
            conn->recv_window = TCP_RX_BUFFER_SIZE;
            conn->mss = 1460;
            conn->rx_buffer_len = 0;
            conn->tx_buffer_len = 0;
            conn->retransmit_count = 0;
            conn->cwnd = 1;
            conn->ssthresh = 65535;
            conn->in_slow_start = true;
            conn->last_activity = tcp_get_time();
            conn->pending_accept = false;
            conn->valid = true;
            return conn;
        }
    }
    return NULL;
}

struct tcp_connection* tcp_connection_accept_pending(uint16_t local_port,
                                                     struct tcp_connection* listen_conn) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (&tcp_connections[i] == listen_conn) continue;
        if (tcp_connections[i].valid &&
            tcp_connections[i].state == TCP_ESTABLISHED &&
            tcp_connections[i].src_port == local_port &&
            tcp_connections[i].dest_ip != 0 &&
            tcp_connections[i].pending_accept) {
            return &tcp_connections[i];
        }
    }
    return NULL;
}

// Отправить данные
int tcp_connection_send(struct tcp_connection* conn, const void* data, size_t len) {
    if (!conn || conn->state != TCP_ESTABLISHED) return -1;
    if (len > TCP_TX_BUFFER_SIZE - conn->tx_buffer_len) return -1;
    
    // Копируем данные в буфер
    const uint8_t* src = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        conn->tx_buffer[conn->tx_buffer_len++] = src[i];
    }
    
    return len;
}

// Получить данные
int tcp_connection_receive(struct tcp_connection* conn, void* buffer, size_t max_len) {
    if (!conn || conn->rx_buffer_len == 0) return 0;
    
    size_t to_copy = (conn->rx_buffer_len < max_len) ? conn->rx_buffer_len : max_len;
    uint8_t* dst = (uint8_t*)buffer;
    
    for (size_t i = 0; i < to_copy; i++) {
        dst[i] = conn->rx_buffer[i];
    }
    
    // Сдвигаем буфер
    for (size_t i = to_copy; i < conn->rx_buffer_len; i++) {
        conn->rx_buffer[i - to_copy] = conn->rx_buffer[i];
    }
    conn->rx_buffer_len -= to_copy;
    
    return to_copy;
}

// Закрыть соединение
void tcp_connection_close(struct tcp_connection* conn) {
    if (!conn) return;
    conn->state = TCP_CLOSED;
    conn->valid = false;
}

// Добавить сегмент в очередь retransmission
void tcp_add_retransmit(struct tcp_connection* conn, uint32_t seq, size_t len, const void* data) {
    if (!conn || conn->retransmit_count >= TCP_MAX_RETRANSMIT) return;
    
    int idx = conn->retransmit_count++;
    conn->retransmit_queue[idx].seq = seq;
    conn->retransmit_queue[idx].len = len;
    conn->retransmit_queue[idx].rto = TCP_RTO_INITIAL;
    conn->retransmit_queue[idx].retries = 0;
    conn->retransmit_queue[idx].timeout = tcp_get_time() + TCP_RTO_INITIAL;
    
    // Копируем данные
    if (data && len > 0 && len <= TCP_MAX_PAYLOAD) {
        const uint8_t* src = (const uint8_t*)data;
        for (size_t i = 0; i < len; i++) {
            conn->retransmit_queue[idx].data[i] = src[i];
        }
    }
}

// Удалить подтвержденные сегменты
void tcp_ack_retransmit(struct tcp_connection* conn, uint32_t ack_num) {
    if (!conn) return;
    
    int write_idx = 0;
    for (int i = 0; i < conn->retransmit_count; i++) {
        // Если сегмент подтвержден (ack_num > seq + len), пропускаем его
        if (ack_num <= conn->retransmit_queue[i].seq + conn->retransmit_queue[i].len) {
            // Сохраняем неподтвержденный сегмент
            if (write_idx != i) {
                conn->retransmit_queue[write_idx] = conn->retransmit_queue[i];
            }
            write_idx++;
        }
    }
    conn->retransmit_count = write_idx;
}

// Обработать таймеры
void tcp_process_timers() {
    tcp_increment_time();
    
    // Объявляем функцию для retransmission (из tcp.h)
    extern int tcp_send(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port,
                       uint32_t seq_num, uint32_t ack_num,
                       uint8_t flags, uint16_t window,
                       const void* payload, size_t payload_size);
    
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        struct tcp_connection* conn = &tcp_connections[i];
        if (!conn->valid) continue;
        
        // Обработка retransmission
        for (int j = 0; j < conn->retransmit_count; j++) {
            if (tcp_get_time() >= conn->retransmit_queue[j].timeout) {
                // Таймаут - повторная отправка
                if (conn->retransmit_queue[j].retries < TCP_MAX_RETRANSMIT) {
                    // Повторная отправка
                    tcp_send(conn->dest_ip, conn->src_port, conn->dest_port,
                            conn->retransmit_queue[j].seq, conn->recv_seq,
                            TCP_FLAG_ACK, conn->recv_window,
                            conn->retransmit_queue[j].data, conn->retransmit_queue[j].len);
                    
                    conn->retransmit_queue[j].retries++;
                    conn->retransmit_queue[j].rto *= 2;  // Exponential backoff
                    if (conn->retransmit_queue[j].rto > TCP_RTO_MAX) {
                        conn->retransmit_queue[j].rto = TCP_RTO_MAX;
                    }
                    conn->retransmit_queue[j].timeout = tcp_get_time() + conn->retransmit_queue[j].rto;
                } else {
                    // Превышено количество попыток - закрываем соединение
                    tcp_connection_close(conn);
                }
            }
        }
    }
}

void tcp_foreach_connection(tcp_conn_fn fn, void* userdata) {
    if (!fn) return;
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (tcp_connections[i].valid) {
            fn(&tcp_connections[i], userdata);
        }
    }
}
