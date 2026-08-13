#ifndef SOCKET_H
#define SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
};

void socket_init(void);

void socket_service_network(void);

int socket_create(int domain, int type, int protocol);
int socket_bind(int fd, const struct sockaddr_in* addr);
int socket_listen(int fd, int backlog);
int socket_accept(int fd, int timeout_ms);
int socket_connect(int fd, const struct sockaddr_in* addr, int timeout_ms);
int socket_send(int fd, const void* buf, size_t len);
int socket_recv(int fd, void* buf, size_t len, int timeout_ms);
int socket_close(int fd);

/* Close every socket owned by pid. Returns number closed. */
int socket_close_by_pid(int pid);

/* Tag socket with service name / synthetic PID (shown in ports/netstat) */
/* pid < 0 => use sched_current_id(); pid >= 0 stored as-is (0 = shell). */
int socket_set_owner(int fd, const char* name, int pid);
int socket_getsockname(int fd, struct sockaddr_in* addr);

int socket_get_peer(int fd, uint32_t* ip, uint16_t* port);

size_t socket_recv_available(int fd);

int socket_recv_exact(int fd, void* buf, size_t need, int timeout_ms);

bool socket_udp_deliver(uint16_t dst_port, uint32_t src_ip, uint16_t src_port,
                        const void* data, size_t len);

#endif
