#include "socket.h"
#include "protocols/tcp.h"
#include "protocols/tcp_connection.h"
#include "protocols/udp.h"
#include "protocols/ip.h"
#include "nic.h"
#include "dhcp/dhcp.h"
#include <stddef.h>
#include <stdint.h>

#define SOCKET_MAX 32
#define UDP_SOCK_QUEUE 8
#define UDP_PKT_MAX  512

struct udp_sock_pkt {
    uint32_t src_ip;
    uint16_t src_port;
    uint8_t data[UDP_PKT_MAX];
    size_t len;
};

struct socket_entry {
    bool used;
    int type;
    uint16_t local_port;
    uint32_t local_ip;
    uint32_t peer_ip;
    uint16_t peer_port;
    bool has_peer;
    bool listening;
    struct tcp_connection* tcp;
    struct tcp_connection* listen_tcp;
    struct udp_sock_pkt udp_q[UDP_SOCK_QUEUE];
    int udp_q_head;
    int udp_q_tail;
    int udp_q_count;
};

static struct socket_entry sockets[SOCKET_MAX];

static void socket_reset(struct socket_entry* s) {
    s->used = false;
    s->type = 0;
    s->local_port = 0;
    s->local_ip = 0;
    s->peer_ip = 0;
    s->peer_port = 0;
    s->has_peer = false;
    s->listening = false;
    s->tcp = 0;
    s->listen_tcp = 0;
    s->udp_q_head = 0;
    s->udp_q_tail = 0;
    s->udp_q_count = 0;
}

void socket_init(void) {
    for (int i = 0; i < SOCKET_MAX; i++) {
        socket_reset(&sockets[i]);
    }
}

void socket_service_network(void) {
    nic_process_packets();
    tcp_process_timers();
    dhcp_poll();
}

static struct socket_entry* socket_get(int fd) {
    if (fd < 0 || fd >= SOCKET_MAX) return 0;
    if (!sockets[fd].used) return 0;
    return &sockets[fd];
}

static int socket_alloc(void) {
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (!sockets[i].used) {
            socket_reset(&sockets[i]);
            sockets[i].used = true;
            return i;
        }
    }
    return -1;
}

static struct socket_entry* socket_find_udp_port(uint16_t port) {
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (sockets[i].used && sockets[i].type == SOCK_DGRAM && sockets[i].local_port == port) {
            return &sockets[i];
        }
    }
    return 0;
}

static void socket_delay(void) {
    for (volatile int j = 0; j < 80000; j++);
}

int socket_create(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET) return -1;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -1;
    int fd = socket_alloc();
    if (fd < 0) return -1;
    sockets[fd].type = type;
    return fd;
}

int socket_bind(int fd, const struct sockaddr_in* addr) {
    struct socket_entry* s = socket_get(fd);
    if (!s || !addr) return -1;
    if (addr->sin_port == 0) return -1;

    for (int i = 0; i < SOCKET_MAX; i++) {
        if (i == fd || !sockets[i].used) continue;
        if (sockets[i].local_port == addr->sin_port) return -1;
    }

    s->local_port = addr->sin_port;
    s->local_ip = addr->sin_addr ? addr->sin_addr : ip_get_our_ip();
    return 0;
}

int socket_listen(int fd, int backlog) {
    (void)backlog;
    struct socket_entry* s = socket_get(fd);
    if (!s || s->type != SOCK_STREAM) return -1;
    if (s->local_port == 0) return -1;
    if (s->listen_tcp) return 0;

    struct tcp_connection* lc = tcp_listen(s->local_port);
    if (!lc) return -1;
    s->listen_tcp = lc;
    s->listening = true;
    return 0;
}

int socket_accept(int fd, int timeout_ms) {
    struct socket_entry* s = socket_get(fd);
    if (!s || s->type != SOCK_STREAM || !s->listen_tcp) return -1;

    int attempts = timeout_ms > 0 ? timeout_ms / 50 : 200;
    if (attempts < 1) attempts = 1;

    for (int i = 0; i < attempts; i++) {
        socket_service_network();
        struct tcp_connection* accepted = tcp_connection_accept_pending(s->local_port, s->listen_tcp);
        if (accepted) {
            accepted->pending_accept = false;
            int cfd = socket_alloc();
            if (cfd < 0) {
                accepted->pending_accept = true;
                return -1;
            }
            struct socket_entry* cs = &sockets[cfd];
            cs->type = SOCK_STREAM;
            cs->local_port = s->local_port;
            cs->local_ip = s->local_ip;
            cs->peer_ip = accepted->dest_ip;
            cs->peer_port = accepted->dest_port;
            cs->has_peer = true;
            cs->tcp = accepted;
            return cfd;
        }
        socket_delay();
    }
    return -1;
}

int socket_connect(int fd, const struct sockaddr_in* addr, int timeout_ms) {
    struct socket_entry* s = socket_get(fd);
    if (!s || !addr) return -1;
    if (addr->sin_addr == 0 || addr->sin_port == 0) return -1;

    s->peer_ip = addr->sin_addr;
    s->peer_port = addr->sin_port;
    s->has_peer = true;

    if (s->type == SOCK_DGRAM) {
        if (s->local_port == 0) {
            s->local_port = (uint16_t)(49152 + (fd * 137) % 16384);
        }
        return 0;
    }

    if (s->tcp) return -1;

    struct tcp_connection* conn = tcp_connect(addr->sin_addr, addr->sin_port);
    if (!conn) return -1;

    if (tcp_connect_wait(conn, timeout_ms > 0 ? timeout_ms : 5000) != 0) {
        tcp_close(conn);
        return -1;
    }

    s->tcp = conn;
    s->local_port = conn->src_port;
    s->local_ip = conn->src_ip;
    return 0;
}

int socket_send(int fd, const void* buf, size_t len) {
    struct socket_entry* s = socket_get(fd);
    if (!s || !buf || len == 0) return -1;

    if (s->type == SOCK_DGRAM) {
        if (!s->has_peer) return -1;
        if (s->local_port == 0) {
            s->local_port = (uint16_t)(49152 + (fd * 137) % 16384);
        }
        if (udp_send(s->peer_ip, s->local_port, s->peer_port, buf, len) != 0) return -1;
        return (int)len;
    }

    if (!s->tcp || s->tcp->state != TCP_ESTABLISHED) return -1;
    int sent = tcp_send_data(s->tcp, buf, len);
    return sent >= 0 ? sent : -1;
}

static int socket_tcp_try_recv(struct socket_entry* s, void* buf, size_t len) {
    if (!s->tcp) return -1;
    int n = tcp_recv_data(s->tcp, buf, len);
    if (n > 0) return n;
    if (s->tcp->state == TCP_CLOSED || !s->tcp->valid) return -1;
    return 0;
}

static int socket_udp_try_recv(struct socket_entry* s, void* buf, size_t len) {
    if (s->udp_q_count == 0) return 0;
    struct udp_sock_pkt* p = &s->udp_q[s->udp_q_head];
    size_t n = p->len < len ? p->len : len;
    for (size_t i = 0; i < n; i++) {
        ((uint8_t*)buf)[i] = p->data[i];
    }
    s->peer_ip = p->src_ip;
    s->peer_port = p->src_port;
    s->has_peer = true;
    s->udp_q_head = (s->udp_q_head + 1) % UDP_SOCK_QUEUE;
    s->udp_q_count--;
    return (int)n;
}

int socket_recv(int fd, void* buf, size_t len, int timeout_ms) {
    struct socket_entry* s = socket_get(fd);
    if (!s || !buf || len == 0) return -1;

    int attempts = timeout_ms > 0 ? timeout_ms / 50 : 1;
    if (attempts < 1) attempts = 1;

    for (int i = 0; i < attempts; i++) {
        int n = (s->type == SOCK_DGRAM) ? socket_udp_try_recv(s, buf, len)
                                        : socket_tcp_try_recv(s, buf, len);
        if (n != 0) return n;
        socket_service_network();
        socket_delay();
    }
    return 0;
}

int socket_close(int fd) {
    struct socket_entry* s = socket_get(fd);
    if (!s) return -1;

    if (s->tcp) {
        tcp_close(s->tcp);
        s->tcp = 0;
    }
    if (s->listen_tcp) {
        tcp_connection_close(s->listen_tcp);
        s->listen_tcp = 0;
    }
    socket_reset(s);
    return 0;
}

int socket_get_peer(int fd, uint32_t* ip, uint16_t* port) {
    struct socket_entry* s = socket_get(fd);
    if (!s || !s->has_peer) return -1;
    if (ip) *ip = s->peer_ip;
    if (port) *port = s->peer_port;
    return 0;
}

size_t socket_recv_available(int fd) {
    struct socket_entry* s = socket_get(fd);
    if (!s || s->type != SOCK_STREAM || !s->tcp) return 0;
    return s->tcp->rx_buffer_len;
}

int socket_recv_exact(int fd, void* buf, size_t need, int timeout_ms) {
    if (!buf || need == 0) return 0;
    uint8_t* out = (uint8_t*)buf;
    size_t got = 0;
    int idle = 0;
    int max_idle = timeout_ms > 0 ? (timeout_ms / 50) + 1 : 40;
    if (max_idle < 8) max_idle = 8;
    if (max_idle > 120) max_idle = 120;

    while (got < need) {
        size_t avail = socket_recv_available(fd);
        if (avail > 0) {
            size_t chunk = need - got;
            if (chunk > avail) chunk = avail;
            int n = socket_recv(fd, out + got, chunk, 50);
            if (n > 0) {
                got += (size_t)n;
                idle = 0;
                continue;
            }
            if (n < 0) return -1;
        }
        idle++;
        if (idle >= max_idle) break;
        socket_service_network();
    }
    return (got == need) ? 0 : -1;
}

bool socket_udp_deliver(uint16_t dst_port, uint32_t src_ip, uint16_t src_port,
                        const void* data, size_t len) {
    struct socket_entry* s = socket_find_udp_port(dst_port);
    if (!s) return false;
    if (len > UDP_PKT_MAX) len = UDP_PKT_MAX;
    if (s->udp_q_count >= UDP_SOCK_QUEUE) {
        s->udp_q_head = (s->udp_q_head + 1) % UDP_SOCK_QUEUE;
        s->udp_q_count--;
    }
    struct udp_sock_pkt* p = &s->udp_q[s->udp_q_tail];
    p->src_ip = src_ip;
    p->src_port = src_port;
    p->len = len;
    for (size_t i = 0; i < len; i++) {
        p->data[i] = ((const uint8_t*)data)[i];
    }
    s->udp_q_tail = (s->udp_q_tail + 1) % UDP_SOCK_QUEUE;
    s->udp_q_count++;
    return true;
}
