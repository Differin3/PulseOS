#ifndef NET_PORTS_H
#define NET_PORTS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NET_PROTO_TCP 6
#define NET_PROTO_UDP 17

#define NET_PORTS_MAX   64
#define NET_OWNER_MAX   24

#define NET_PID_SHELL   1
#define NET_PID_DHCPD   68
#define NET_PID_HTTPD   80
#define NET_PID_DNS     53
#define NET_PID_SOCKTEST 900

enum net_port_state {
    NETPORT_CLOSED = 0,
    NETPORT_LISTEN,
    NETPORT_SYN_SENT,
    NETPORT_ESTABLISHED,
    NETPORT_TIME_WAIT,
    NETPORT_OTHER
};

struct net_port_info {
    uint8_t proto;
    uint8_t state;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t local_ip;
    uint32_t remote_ip;
    int sock_fd;
    int pid;
    char owner[NET_OWNER_MAX];
};

void net_ports_init(void);

/* Ephemeral port in 49152..65535, or 0 on failure */
uint16_t net_ports_alloc_ephemeral(uint8_t proto);

bool net_ports_busy(uint8_t proto, uint16_t port);

/* Register / update a binding. sock_fd=-1 for kernel services. Returns 0 ok. */
int net_ports_register(uint8_t proto, uint8_t state,
                       uint32_t local_ip, uint16_t local_port,
                       uint32_t remote_ip, uint16_t remote_port,
                       int sock_fd, int pid, const char* owner);

void net_ports_set_state(uint8_t proto, uint16_t local_port,
                         uint32_t remote_ip, uint16_t remote_port,
                         uint8_t state);

void net_ports_release_sock(int sock_fd);
void net_ports_release_listen(uint8_t proto, uint16_t port);

/* Close a listening port (socket or kernel listen). 0 = ok */
int net_ports_close_listen(uint8_t proto, uint16_t port);

bool net_ports_lookup_owner(uint8_t proto, uint16_t local_port,
                            char* owner_out, size_t owner_cap, int* pid_out);

typedef void (*net_port_fn)(const struct net_port_info* info, void* userdata);
void net_ports_foreach(net_port_fn fn, void* userdata);

/* Merge live TCP table into the registry for display (owners by local port). */
void net_ports_sync_tcp(void);

const char* net_port_state_str(uint8_t state);
const char* net_port_proto_str(uint8_t proto);

/* Write human-readable table into buf. Returns bytes written (no NUL in count; NUL added if room). */
size_t net_ports_format_table(char* buf, size_t cap);

/* Guest CI: dhcpd row, bind/listen/owner, ephemeral, close. 0 = ok */
int net_ports_autotest(void);

#endif
