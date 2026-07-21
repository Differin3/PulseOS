#include "tcp.h"
#include "tcp_connection.h"
#include "ip.h"
#include "ethernet.h"
#include "arp.h"
#include "../nic.h"  // для ETH_MAX_PACKET_SIZE и nic_get_mac
#include "../../../serial_log.h"
#include <stddef.h>

// ISN объявлен в tcp_connection.cpp
extern uint32_t tcp_next_isn;

// Функции из tcp_connection.cpp
extern void tcp_add_retransmit(struct tcp_connection* conn, uint32_t seq, size_t len, const void* data);
extern void tcp_ack_retransmit(struct tcp_connection* conn, uint32_t ack_num);

// Преобразование host byte order в network byte order
static inline uint16_t htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort >> 8) & 0xFF);
}

static inline uint32_t htonl(uint32_t hostlong) {
    return ((hostlong & 0xFF) << 24) | ((hostlong & 0xFF00) << 8) |
           ((hostlong & 0xFF0000) >> 8) | ((hostlong & 0xFF000000) >> 24);
}

// Преобразование network byte order в host byte order
static inline uint16_t ntohs(uint16_t netshort) {
    return ((netshort & 0xFF) << 8) | ((netshort >> 8) & 0xFF);
}

static inline uint32_t ntohl(uint32_t netlong) {
    return ((netlong & 0xFF) << 24) | ((netlong & 0xFF00) << 8) |
           ((netlong & 0xFF0000) >> 8) | ((netlong & 0xFF000000) >> 24);
}

// IP в pseudo-header checksum (как в udp.cpp)
static inline uint32_t tcp_ip_for_checksum(uint32_t ip) {
    return ((ip & 0xFF) << 24) | ((ip & 0xFF00) << 8) |
           ((ip & 0xFF0000) >> 8) | ((ip & 0xFF000000) >> 24);
}

// Вычислить TCP checksum
uint16_t tcp_checksum(uint32_t src_ip, uint32_t dest_ip,
                     const struct tcp_header* header, size_t tcp_len) {
    // Pseudo-header для checksum
    struct {
        uint32_t src_ip;
        uint32_t dest_ip;
        uint8_t zero;
        uint8_t protocol;
        uint16_t tcp_len;
    } __attribute__((packed)) pseudo_header;
    
    pseudo_header.src_ip = tcp_ip_for_checksum(src_ip);
    pseudo_header.dest_ip = tcp_ip_for_checksum(dest_ip);
    pseudo_header.zero = 0;
    pseudo_header.protocol = IP_PROTOCOL_TCP;
    pseudo_header.tcp_len = htons((uint16_t)tcp_len);
    
    // Суммируем pseudo-header (по байтам для избежания проблем с выравниванием)
    uint32_t sum = 0;
    const uint8_t* bytes = (const uint8_t*)&pseudo_header;
    for (size_t i = 0; i < sizeof(pseudo_header); i += 2) {
        if (i + 1 < sizeof(pseudo_header)) {
            sum += ((uint16_t)bytes[i] << 8) | bytes[i + 1];
        } else {
            sum += (uint16_t)bytes[i] << 8;
        }
    }
    
    // Суммируем TCP заголовок и данные (по байтам)
    bytes = (const uint8_t*)header;
    for (size_t i = 0; i < tcp_len; i += 2) {
        if (i + 1 < tcp_len) {
            sum += ((uint16_t)bytes[i] << 8) | bytes[i + 1];
        } else {
            sum += (uint16_t)bytes[i] << 8;
        }
    }
    
    // Складываем переносы
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return (uint16_t)~sum;
}

// Создать TCP сегмент
int tcp_create_packet(void* buffer, size_t buffer_size,
                     uint16_t src_port, uint16_t dest_port,
                     uint32_t seq_num, uint32_t ack_num,
                     uint8_t flags, uint16_t window,
                     const void* payload, size_t payload_size) {
    if (!buffer || buffer_size < sizeof(struct tcp_header) + payload_size) {
        return -1;
    }
    
    struct tcp_header* header = (struct tcp_header*)buffer;
    
    header->src_port = htons(src_port);
    header->dest_port = htons(dest_port);
    header->seq_num = htonl(seq_num);
    header->ack_num = htonl(ack_num);
    header->data_offset = (sizeof(struct tcp_header) / 4) << 4;  // 5 * 4 = 20 байт
    header->flags = flags;
    header->window = htons(window);
    header->checksum = 0;  // Вычислим позже
    header->urgent_ptr = 0;
    
    // Копируем payload
    if (payload && payload_size > 0) {
        uint8_t* payload_ptr = (uint8_t*)buffer + sizeof(struct tcp_header);
        const uint8_t* src = (const uint8_t*)payload;
        for (size_t i = 0; i < payload_size; i++) {
            payload_ptr[i] = src[i];
        }
    }
    
    return sizeof(struct tcp_header) + payload_size;
}

// Парсить TCP заголовок
int tcp_parse_header(const void* packet, size_t packet_size,
                    struct tcp_header* header, const void** payload, size_t* payload_size) {
    if (!packet || packet_size < sizeof(struct tcp_header)) {
        return -1;
    }
    
    const struct tcp_header* hdr = (const struct tcp_header*)packet;
    
    if (header) {
        *header = *hdr;
    }
    
    // Получаем data offset (в 32-битных словах)
    uint8_t data_offset = (hdr->data_offset >> 4) * 4;
    if (data_offset < sizeof(struct tcp_header) || data_offset > packet_size) {
        return -1;
    }
    
    if (payload) {
        *payload = (const uint8_t*)packet + data_offset;
    }
    
    if (payload_size) {
        *payload_size = packet_size - data_offset;
    }
    
    return 0;
}

// Отправить TCP сегмент через IP
int tcp_send(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port,
            uint32_t seq_num, uint32_t ack_num,
            uint8_t flags, uint16_t window,
            const void* payload, size_t payload_size) {
    if (payload_size > TCP_MAX_PAYLOAD) {
        return -1;
    }
    
    // Создаем TCP сегмент
    uint8_t tcp_buffer[sizeof(struct tcp_header) + TCP_MAX_PAYLOAD];
    int tcp_len = tcp_create_packet(tcp_buffer, sizeof(tcp_buffer),
                                    src_port, dest_port,
                                    seq_num, ack_num, flags, window,
                                    payload, payload_size);
    if (tcp_len < 0) return -1;
    
    // Вычисляем checksum
    uint32_t src_ip = ip_get_our_ip();
    struct tcp_header* tcp_hdr = (struct tcp_header*)tcp_buffer;
    tcp_hdr->checksum = htons(tcp_checksum(src_ip, dest_ip, tcp_hdr, tcp_len));
    
    // Создаем IP пакет
    uint8_t ip_buffer[sizeof(struct ip_header) + sizeof(tcp_buffer)];
    int ip_len = ip_create_packet(ip_buffer, sizeof(ip_buffer),
                                 src_ip, dest_ip, IP_PROTOCOL_TCP,
                                 tcp_buffer, tcp_len);
    if (ip_len < 0) return -1;
    
    // Получаем MAC адрес через ARP
    uint8_t dest_mac[6];
    if (arp_resolve(ip_resolve_next_hop(dest_ip), dest_mac, 3000) != 0) {
        return -1;
    }
    
    // Создаем Ethernet фрейм
    uint8_t our_mac[6];
    nic_get_mac(our_mac);
    uint8_t frame[sizeof(struct ethernet_header) + sizeof(ip_buffer)];
    int frame_len = ethernet_create_frame(frame, sizeof(frame),
                                         dest_mac, our_mac,
                                         ETH_TYPE_IPV4, ip_buffer, ip_len);
    if (frame_len < 0) return -1;
    
    // Отправляем через NIC
    return nic_send_packet(frame, frame_len);
}

// Обработать входящий TCP пакет
void tcp_handle_packet(uint32_t src_ip, uint32_t dest_ip,
                      const void* ip_payload, size_t payload_size) {
    if (!ip_payload || payload_size < sizeof(struct tcp_header)) {
        return;
    }
    
    struct tcp_header tcp_hdr;
    const void* tcp_payload;
    size_t tcp_payload_size;
    
    if (tcp_parse_header(ip_payload, payload_size, &tcp_hdr, &tcp_payload, &tcp_payload_size) != 0) {
        return;
    }
    
    // Конвертируем из network byte order
    uint16_t src_port = ntohs(tcp_hdr.src_port);
    uint16_t dest_port = ntohs(tcp_hdr.dest_port);
    uint32_t seq_num = ntohl(tcp_hdr.seq_num);
    uint32_t ack_num = ntohl(tcp_hdr.ack_num);
    uint16_t window = ntohs(tcp_hdr.window);

    uint32_t our_ip = ip_get_our_ip();
    if (dest_ip != our_ip) return;

    uint8_t hdr_len = (tcp_hdr.data_offset >> 4) * 4;
    if (hdr_len < sizeof(struct tcp_header)) return;
    size_t tcp_len = hdr_len + tcp_payload_size;
    if (tcp_hdr.checksum != 0) {
        // Inclusive verify (RFC 1071): sum over segment with checksum field included == 0xFFFF
        uint16_t inv = tcp_checksum(src_ip, dest_ip,
                                    (const struct tcp_header*)ip_payload, tcp_len);
        if (inv != 0) {
            uint16_t got = ((const uint8_t*)ip_payload)[16];
            got = (uint16_t)((got << 8) | ((const uint8_t*)ip_payload)[17]);
            log_fmt3(LOG_INFO, "tcp", "csum fail", "sport", (uint32_t)src_port,
                     "dport", (uint32_t)dest_port, "inv", (uint32_t)inv);
            log_fmt3(LOG_DBG, "tcp", "csum detail", "got", (uint32_t)got,
                     "len", (uint32_t)tcp_len, "flags", (uint32_t)tcp_hdr.flags);
            return;
        }
    }

    struct tcp_connection* conn = tcp_connection_find(our_ip, dest_port, src_ip, src_port);

    if (tcp_hdr.flags & TCP_FLAG_RST) {
        if (conn) tcp_connection_close(conn);
        return;
    }

    // Обработка по флагам
    if (tcp_hdr.flags & TCP_FLAG_SYN) {
        log_fmt3(LOG_INFO, "tcp", "syn rx", "sport", (uint32_t)src_port,
                 "dport", (uint32_t)dest_port, "flags", (uint32_t)tcp_hdr.flags);
        struct tcp_connection* listen_conn = tcp_connection_find_listen(dest_port);
        if (!conn && listen_conn) {
            struct tcp_connection* new_conn = tcp_connection_create(our_ip, dest_port, src_ip, src_port);
            if (new_conn) {
                new_conn->state = TCP_SYN_RECEIVED;
                new_conn->recv_seq = seq_num + 1;

                log_msg(LOG_INFO, "tcp", "syn-ack sent");
                tcp_send(src_ip, dest_port, src_port,
                        new_conn->send_seq, new_conn->recv_seq,
                        TCP_FLAG_SYN | TCP_FLAG_ACK, new_conn->recv_window,
                        NULL, 0);
                new_conn->send_seq++;
            }
        } else if (!conn && !listen_conn) {
            log_fmt3(LOG_INFO, "tcp", "syn no listen", "sport", (uint32_t)src_port,
                     "dport", (uint32_t)dest_port, "flags", (uint32_t)tcp_hdr.flags);
        } else if (conn && conn->state == TCP_SYN_SENT) {
            // Ответ на наш SYN
            conn->state = TCP_ESTABLISHED;
            conn->recv_seq = seq_num + 1;
            
            // Отправляем ACK
            tcp_send(src_ip, dest_port, src_port,
                    conn->send_seq, conn->recv_seq,
                    TCP_FLAG_ACK, conn->recv_window,
                    NULL, 0);
        }
    } else if (tcp_hdr.flags & TCP_FLAG_ACK) {
        if (conn) {
            if (conn->state == TCP_SYN_RECEIVED) {
                conn->state = TCP_ESTABLISHED;
                conn->pending_accept = true;
                log_fmt3(LOG_INFO, "tcp", "established", "sport", (uint32_t)src_port,
                         "dport", (uint32_t)dest_port, "accept", 1u);
            }
            if (conn->state == TCP_FIN_WAIT_1 && ack_num == conn->send_seq) {
                conn->state = TCP_FIN_WAIT_2;
            }
            
            // Обновляем send_una
            if (ack_num > conn->send_una) {
                uint32_t acked_bytes = ack_num - conn->send_una;
                conn->send_una = ack_num;
                
                // Удаляем подтвержденные сегменты из очереди retransmission
                tcp_ack_retransmit(conn, ack_num);
                
                // Congestion control: увеличиваем окно при успешном ACK
                if (conn->in_slow_start) {
                    // Slow start
                    conn->cwnd += acked_bytes;
                    if (conn->cwnd >= conn->ssthresh) {
                        conn->in_slow_start = false;
                    }
                } else {
                    // Congestion avoidance
                    conn->cwnd += (acked_bytes * acked_bytes) / conn->cwnd + 1;
                }
            }
            
            // Обновляем окно (flow control)
            conn->send_window = window;
        }
    }
    
    // Обработка данных
    if (tcp_payload_size > 0 && conn && conn->state == TCP_ESTABLISHED) {
        if (seq_num == conn->recv_seq) {
            // Правильная последовательность
            if (conn->rx_buffer_len + tcp_payload_size <= TCP_RX_BUFFER_SIZE) {
                // Копируем данные в буфер
                const uint8_t* src = (const uint8_t*)tcp_payload;
                for (size_t i = 0; i < tcp_payload_size; i++) {
                    conn->rx_buffer[conn->rx_buffer_len++] = src[i];
                }
                conn->recv_seq += tcp_payload_size;
                
                // Обновляем receive window (сколько места осталось в буфере)
                conn->recv_window = TCP_RX_BUFFER_SIZE - conn->rx_buffer_len;
                
                // Отправляем ACK
                tcp_send(src_ip, dest_port, src_port,
                        conn->send_seq, conn->recv_seq,
                        TCP_FLAG_ACK, conn->recv_window,
                        NULL, 0);
            }
        }
    }
    
    // Обработка FIN
    if (tcp_hdr.flags & TCP_FLAG_FIN) {
        if (conn && conn->state == TCP_ESTABLISHED) {
            conn->state = TCP_CLOSE_WAIT;
            conn->recv_seq++;

            tcp_send(src_ip, dest_port, src_port,
                    conn->send_seq, conn->recv_seq,
                    TCP_FLAG_ACK, conn->recv_window,
                    NULL, 0);
        } else if (conn && conn->state == TCP_FIN_WAIT_2) {
            conn->recv_seq++;
            tcp_send(src_ip, dest_port, src_port,
                    conn->send_seq, conn->recv_seq,
                    TCP_FLAG_ACK, conn->recv_window,
                    NULL, 0);
            tcp_connection_close(conn);
        }
    }
}

// TCP API для приложений

// Инициализация TCP стека
void tcp_init() {
    tcp_connection_init();
}

// Подключиться к удаленному хосту
struct tcp_connection* tcp_connect(uint32_t dest_ip, uint16_t dest_port) {
    uint32_t src_ip = ip_get_our_ip();
    if (src_ip == 0) return NULL;
    
    // Выбираем случайный порт источника
    uint16_t src_port = 49152 + (tcp_next_isn % 16384);  // Диапазон 49152-65535
    
    struct tcp_connection* conn = tcp_connection_create(src_ip, src_port, dest_ip, dest_port);
    if (!conn) return NULL;
    
    // Отправляем SYN
    tcp_send(dest_ip, src_port, dest_port,
            conn->send_seq, 0,
            TCP_FLAG_SYN, conn->recv_window,
            NULL, 0);
    
    conn->send_seq++;
    
    return conn;
}

// Дождаться установления TCP соединения
int tcp_connect_wait(struct tcp_connection* conn, int timeout_ms) {
    if (!conn) return -1;
    if (conn->state == TCP_ESTABLISHED) return 0;
    
    int attempts = timeout_ms > 0 ? timeout_ms / 100 : 50;
    if (attempts < 1) attempts = 1;
    
    for (int i = 0; i < attempts; i++) {
        nic_process_packets();
        tcp_process_timers();
        
        if (conn->state == TCP_ESTABLISHED) {
            return 0;
        }
        if (conn->state == TCP_CLOSED) {
            return -1;
        }
        
        for (volatile int j = 0; j < 100000; j++);
    }
    
    return -1;
}

// Прослушивать порт
struct tcp_connection* tcp_listen(uint16_t port) {
    uint32_t src_ip = ip_get_our_ip();
    if (src_ip == 0) return NULL;

    if (tcp_connection_find_listen(port)) {
        return NULL;
    }

    return tcp_connection_alloc_listen(src_ip, port);
}

struct tcp_connection* tcp_accept(struct tcp_connection* listen_conn, int timeout_ms) {
    if (!listen_conn || listen_conn->state != TCP_LISTEN) {
        return NULL;
    }

    int attempts = timeout_ms > 0 ? timeout_ms / 100 : 50;
    if (attempts < 1) attempts = 1;

    for (int i = 0; i < attempts; i++) {
        nic_process_packets();
        tcp_process_timers();

        struct tcp_connection* accepted = tcp_connection_accept_pending(
            listen_conn->src_port, listen_conn);
        if (accepted) {
            return accepted;
        }

        for (volatile int j = 0; j < 100000; j++);
    }

    return NULL;
}

// Отправить данные
int tcp_send_data(struct tcp_connection* conn, const void* data, size_t len) {
    if (!conn || conn->state != TCP_ESTABLISHED) return -1;
    
    // Добавляем данные в буфер отправки
    int result = tcp_connection_send(conn, data, len);
    if (result < 0) return -1;
    
    // Отправляем данные по частям (MSS)
    size_t sent = 0;
    while (sent < len) {
        size_t to_send = (len - sent > conn->mss) ? conn->mss : (len - sent);
        
        // Flow control: проверяем окно получателя
        uint32_t window_available = conn->send_window - (conn->send_seq - conn->send_una);
        if (window_available == 0) {
            break;  // Окно заполнено
        }
        if (to_send > window_available) {
            to_send = window_available;
        }
        
        // Congestion control: проверяем congestion window
        uint32_t cwnd_available = conn->cwnd - (conn->send_seq - conn->send_una);
        if (cwnd_available == 0) {
            break;  // Congestion window заполнен
        }
        if (to_send > cwnd_available) {
            to_send = cwnd_available;
        }
        
        // Отправляем сегмент
        tcp_send(conn->dest_ip, conn->src_port, conn->dest_port,
                conn->send_seq, conn->recv_seq,
                TCP_FLAG_ACK | TCP_FLAG_PSH, conn->recv_window,
                (const uint8_t*)data + sent, to_send);
        
        // Добавляем в очередь retransmission
        tcp_add_retransmit(conn, conn->send_seq, to_send, (const uint8_t*)data + sent);
        
        conn->send_seq += to_send;
        sent += to_send;
        
        // Congestion control: увеличиваем окно
        if (conn->in_slow_start) {
            // Slow start: удваиваем окно при каждом ACK
            conn->cwnd += to_send;
            if (conn->cwnd >= conn->ssthresh) {
                conn->in_slow_start = false;
            }
        } else {
            // Congestion avoidance: линейное увеличение
            conn->cwnd += (to_send * to_send) / conn->cwnd + 1;
        }
    }
    
    return sent;
}

// Получить данные
int tcp_recv_data(struct tcp_connection* conn, void* buffer, size_t max_len) {
    if (!conn) return -1;
    return tcp_connection_receive(conn, buffer, max_len);
}

// Закрыть соединение
void tcp_close(struct tcp_connection* conn) {
    if (!conn) return;

    if (conn->state == TCP_ESTABLISHED) {
        tcp_send(conn->dest_ip, conn->src_port, conn->dest_port,
                conn->send_seq, conn->recv_seq,
                TCP_FLAG_FIN | TCP_FLAG_ACK, conn->recv_window,
                NULL, 0);

        conn->state = TCP_FIN_WAIT_1;
        conn->send_seq++;

        for (int i = 0; i < 50; i++) {
            nic_process_packets();
            tcp_process_timers();
            if (!conn->valid) return;
            if (conn->state == TCP_FIN_WAIT_2 || conn->state == TCP_CLOSED) {
                break;
            }
            for (volatile int j = 0; j < 50000; j++);
        }
    }

    if (conn->valid) {
        tcp_connection_close(conn);
    }
}
