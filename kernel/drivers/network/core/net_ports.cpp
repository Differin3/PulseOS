#include "net_ports.h"
#include "sched/task.h"
#include "../socket.h"
#include "../protocols/tcp.h"
#include "../protocols/tcp_connection.h"
#include "../protocols/udp.h"
#include "../protocols/ip.h"
#include <stddef.h>

struct net_port_slot {
    bool used;
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

static struct net_port_slot g_ports[NET_PORTS_MAX];
static uint16_t g_ephemeral_next = 49152;

static void np_copy_owner(char* dst, const char* src) {
    size_t i = 0;
    if (!src) src = "?";
    while (src[i] && i + 1 < NET_OWNER_MAX) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool np_same_listen(const struct net_port_slot* s, uint8_t proto, uint16_t port) {
    return s->used && s->proto == proto && s->local_port == port &&
           s->state == NETPORT_LISTEN;
}

void net_ports_init(void) {
    for (int i = 0; i < NET_PORTS_MAX; i++) {
        g_ports[i].used = false;
    }
    g_ephemeral_next = 49152;
}

const char* net_port_state_str(uint8_t state) {
    switch (state) {
        case NETPORT_LISTEN: return "LISTEN";
        case NETPORT_SYN_SENT: return "SYN_SENT";
        case NETPORT_ESTABLISHED: return "ESTABLISHED";
        case NETPORT_TIME_WAIT: return "TIME_WAIT";
        case NETPORT_OTHER: return "OTHER";
        default: return "CLOSED";
    }
}

const char* net_port_proto_str(uint8_t proto) {
    if (proto == NET_PROTO_TCP) return "tcp";
    if (proto == NET_PROTO_UDP) return "udp";
    return "?";
}

bool net_ports_busy(uint8_t proto, uint16_t port) {
    if (port == 0) return false;
    for (int i = 0; i < NET_PORTS_MAX; i++) {
        if (!g_ports[i].used) continue;
        if (g_ports[i].proto != proto) continue;
        if (g_ports[i].local_port == port && g_ports[i].state == NETPORT_LISTEN) {
            return true;
        }
        if (g_ports[i].local_port == port && g_ports[i].remote_port == 0 &&
            g_ports[i].state != NETPORT_CLOSED) {
            return true;
        }
    }
    /* tcp_connection_find_listen only after tcp_init; safe on zeroed BSS if called early */
    if (proto == NET_PROTO_TCP) {
        struct tcp_connection* lc = tcp_connection_find_listen(port);
        if (lc && lc->valid && lc->state == TCP_LISTEN) return true;
    }
    return false;
}

uint16_t net_ports_alloc_ephemeral(uint8_t proto) {
    for (int n = 0; n < 16384; n++) {
        uint16_t p = g_ephemeral_next++;
        if (g_ephemeral_next < 49152) g_ephemeral_next = 49152;
        if (!net_ports_busy(proto, p)) return p;
    }
    return 0;
}

static struct net_port_slot* np_find_free(void) {
    for (int i = 0; i < NET_PORTS_MAX; i++) {
        if (!g_ports[i].used) return &g_ports[i];
    }
    return 0;
}

static struct net_port_slot* np_find_sock(int sock_fd) {
    if (sock_fd < 0) return 0;
    for (int i = 0; i < NET_PORTS_MAX; i++) {
        if (g_ports[i].used && g_ports[i].sock_fd == sock_fd) return &g_ports[i];
    }
    return 0;
}

static struct net_port_slot* np_find_listen(uint8_t proto, uint16_t port) {
    for (int i = 0; i < NET_PORTS_MAX; i++) {
        if (np_same_listen(&g_ports[i], proto, port)) return &g_ports[i];
    }
    return 0;
}

int net_ports_register(uint8_t proto, uint8_t state,
                       uint32_t local_ip, uint16_t local_port,
                       uint32_t remote_ip, uint16_t remote_port,
                       int sock_fd, int pid, const char* owner) {
    if (local_port == 0) return -1;

    struct net_port_slot* s = 0;
    if (sock_fd >= 0) s = np_find_sock(sock_fd);
    if (!s && state == NETPORT_LISTEN) s = np_find_listen(proto, local_port);
    if (!s) {
        /* Match established 4-tuple update */
        for (int i = 0; i < NET_PORTS_MAX; i++) {
            if (!g_ports[i].used) continue;
            if (g_ports[i].proto == proto &&
                g_ports[i].local_port == local_port &&
                g_ports[i].remote_port == remote_port &&
                g_ports[i].remote_ip == remote_ip) {
                s = &g_ports[i];
                break;
            }
        }
    }
    if (!s) s = np_find_free();
    if (!s) return -1;

    if (state == NETPORT_LISTEN && net_ports_busy(proto, local_port) &&
        !np_same_listen(s, proto, local_port) && s->sock_fd != sock_fd) {
        return -1;
    }

    s->used = true;
    s->proto = proto;
    s->state = state;
    s->local_ip = local_ip ? local_ip : ip_get_our_ip();
    s->local_port = local_port;
    s->remote_ip = remote_ip;
    s->remote_port = remote_port;
    s->sock_fd = sock_fd;
    /* pid < 0 => current task; pid >= 0 kept as-is (shell tid is 0). */
    if (pid < 0) {
        int cur = sched_current_id();
        s->pid = cur >= 0 ? cur : 0;
    } else {
        s->pid = pid;
    }
    np_copy_owner(s->owner, owner);
    return 0;
}

void net_ports_set_state(uint8_t proto, uint16_t local_port,
                         uint32_t remote_ip, uint16_t remote_port,
                         uint8_t state) {
    for (int i = 0; i < NET_PORTS_MAX; i++) {
        if (!g_ports[i].used) continue;
        if (g_ports[i].proto != proto) continue;
        if (g_ports[i].local_port != local_port) continue;
        if (remote_port != 0 && g_ports[i].remote_port != remote_port) continue;
        if (remote_ip != 0 && g_ports[i].remote_ip != remote_ip) continue;
        g_ports[i].state = state;
        if (remote_ip) g_ports[i].remote_ip = remote_ip;
        if (remote_port) g_ports[i].remote_port = remote_port;
        return;
    }
}

void net_ports_release_sock(int sock_fd) {
    if (sock_fd < 0) return;
    for (int i = 0; i < NET_PORTS_MAX; i++) {
        if (g_ports[i].used && g_ports[i].sock_fd == sock_fd) {
            g_ports[i].used = false;
        }
    }
}

void net_ports_release_listen(uint8_t proto, uint16_t port) {
    for (int i = 0; i < NET_PORTS_MAX; i++) {
        if (np_same_listen(&g_ports[i], proto, port)) {
            g_ports[i].used = false;
        }
    }
}

int net_ports_close_listen(uint8_t proto, uint16_t port) {
    struct net_port_slot* s = np_find_listen(proto, port);
    if (!s) {
        if (proto == NET_PROTO_TCP) {
            struct tcp_connection* lc = tcp_connection_find_listen(port);
            if (!lc) return -1;
            tcp_connection_close(lc);
            return 0;
        }
        if (proto == NET_PROTO_UDP) {
            udp_unlisten_port(port);
            return 0;
        }
        return -1;
    }

    int fd = s->sock_fd;
    if (fd >= 0) {
        /* socket_close releases the registry entry */
        return socket_close(fd);
    }

    if (proto == NET_PROTO_TCP) {
        struct tcp_connection* lc = tcp_connection_find_listen(port);
        if (lc) tcp_connection_close(lc);
    } else if (proto == NET_PROTO_UDP) {
        udp_unlisten_port(port);
    }
    s->used = false;
    return 0;
}

bool net_ports_lookup_owner(uint8_t proto, uint16_t local_port,
                            char* owner_out, size_t owner_cap, int* pid_out) {
    for (int i = 0; i < NET_PORTS_MAX; i++) {
        if (!g_ports[i].used) continue;
        if (g_ports[i].proto != proto) continue;
        if (g_ports[i].local_port != local_port) continue;
        if (owner_out && owner_cap > 0) {
            size_t j = 0;
            while (g_ports[i].owner[j] && j + 1 < owner_cap) {
                owner_out[j] = g_ports[i].owner[j];
                j++;
            }
            owner_out[j] = 0;
        }
        if (pid_out) *pid_out = g_ports[i].pid;
        return true;
    }
    if (owner_out && owner_cap > 0) {
        owner_out[0] = '-';
        if (owner_cap > 1) owner_out[1] = 0;
    }
    if (pid_out) *pid_out = 0;
    return false;
}

void net_ports_foreach(net_port_fn fn, void* userdata) {
    if (!fn) return;
    for (int i = 0; i < NET_PORTS_MAX; i++) {
        if (!g_ports[i].used) continue;
        struct net_port_info info;
        info.proto = g_ports[i].proto;
        info.state = g_ports[i].state;
        info.local_port = g_ports[i].local_port;
        info.remote_port = g_ports[i].remote_port;
        info.local_ip = g_ports[i].local_ip;
        info.remote_ip = g_ports[i].remote_ip;
        info.sock_fd = g_ports[i].sock_fd;
        info.pid = g_ports[i].pid;
        np_copy_owner(info.owner, g_ports[i].owner);
        fn(&info, userdata);
    }
}

static uint8_t np_map_tcp_state(enum tcp_state st) {
    switch (st) {
        case TCP_LISTEN: return NETPORT_LISTEN;
        case TCP_SYN_SENT: return NETPORT_SYN_SENT;
        case TCP_ESTABLISHED: return NETPORT_ESTABLISHED;
        case TCP_TIME_WAIT: return NETPORT_TIME_WAIT;
        case TCP_CLOSED: return NETPORT_CLOSED;
        default: return NETPORT_OTHER;
    }
}

struct np_sync_ctx {
    int dummy;
};

static bool np_has_tcp_tuple(uint16_t local_port, uint32_t remote_ip, uint16_t remote_port) {
    for (int i = 0; i < NET_PORTS_MAX; i++) {
        if (!g_ports[i].used || g_ports[i].proto != NET_PROTO_TCP) continue;
        if (g_ports[i].local_port != local_port) continue;
        if (g_ports[i].remote_port == remote_port && g_ports[i].remote_ip == remote_ip) {
            return true;
        }
    }
    return false;
}

static void np_sync_one(struct tcp_connection* conn, void* userdata) {
    (void)userdata;
    if (!conn || !conn->valid || conn->state == TCP_CLOSED) return;

    uint8_t st = np_map_tcp_state(conn->state);
    char owner[NET_OWNER_MAX];
    int pid = 0;
    if (!net_ports_lookup_owner(NET_PROTO_TCP, conn->src_port, owner, sizeof(owner), &pid)) {
        np_copy_owner(owner, "kernel");
        int cur = sched_current_id();
        pid = cur >= 0 ? cur : 0;
    }

    if (conn->state == TCP_LISTEN) {
        if (!np_find_listen(NET_PROTO_TCP, conn->src_port)) {
            net_ports_register(NET_PROTO_TCP, NETPORT_LISTEN,
                               conn->src_ip, conn->src_port, 0, 0,
                               -1, pid, owner);
        }
        return;
    }

    if (np_has_tcp_tuple(conn->src_port, conn->dest_ip, conn->dest_port)) {
        net_ports_set_state(NET_PROTO_TCP, conn->src_port,
                            conn->dest_ip, conn->dest_port, st);
        return;
    }

    net_ports_register(NET_PROTO_TCP, st,
                       conn->src_ip, conn->src_port,
                       conn->dest_ip, conn->dest_port,
                       -1, pid, owner);
}

void net_ports_sync_tcp(void) {
    tcp_foreach_connection(np_sync_one, 0);
}

static void np_fmt_putc(char* buf, size_t cap, size_t* len, char c) {
    if (*len + 1 < cap) buf[(*len)++] = c;
}

static void np_fmt_puts(char* buf, size_t cap, size_t* len, const char* s) {
    while (s && *s) np_fmt_putc(buf, cap, len, *s++);
}

static void np_fmt_u32(char* buf, size_t cap, size_t* len, uint32_t v) {
    char tmp[12];
    int t = 0;
    if (v == 0) {
        np_fmt_putc(buf, cap, len, '0');
        return;
    }
    while (v > 0 && t < 11) {
        tmp[t++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (t > 0) np_fmt_putc(buf, cap, len, tmp[--t]);
}

static void np_fmt_ip(char* buf, size_t cap, size_t* len, uint32_t ip) {
    np_fmt_u32(buf, cap, len, (ip >> 24) & 0xFF);
    np_fmt_putc(buf, cap, len, '.');
    np_fmt_u32(buf, cap, len, (ip >> 16) & 0xFF);
    np_fmt_putc(buf, cap, len, '.');
    np_fmt_u32(buf, cap, len, (ip >> 8) & 0xFF);
    np_fmt_putc(buf, cap, len, '.');
    np_fmt_u32(buf, cap, len, ip & 0xFF);
}

struct np_fmt_ctx {
    char* buf;
    size_t cap;
    size_t len;
};

static void np_fmt_one(const struct net_port_info* info, void* userdata) {
    struct np_fmt_ctx* c = (struct np_fmt_ctx*)userdata;
    np_fmt_puts(c->buf, c->cap, &c->len, net_port_proto_str(info->proto));
    np_fmt_putc(c->buf, c->cap, &c->len, ' ');
    np_fmt_ip(c->buf, c->cap, &c->len, info->local_ip);
    np_fmt_putc(c->buf, c->cap, &c->len, ':');
    np_fmt_u32(c->buf, c->cap, &c->len, info->local_port);
    np_fmt_puts(c->buf, c->cap, &c->len, " ");
    if (info->remote_ip == 0 && info->remote_port == 0) {
        np_fmt_puts(c->buf, c->cap, &c->len, "0.0.0.0:*");
    } else {
        np_fmt_ip(c->buf, c->cap, &c->len, info->remote_ip);
        np_fmt_putc(c->buf, c->cap, &c->len, ':');
        np_fmt_u32(c->buf, c->cap, &c->len, info->remote_port);
    }
    np_fmt_puts(c->buf, c->cap, &c->len, " ");
    np_fmt_puts(c->buf, c->cap, &c->len, net_port_state_str(info->state));
    np_fmt_puts(c->buf, c->cap, &c->len, " pid=");
    np_fmt_u32(c->buf, c->cap, &c->len, (uint32_t)info->pid);
    np_fmt_puts(c->buf, c->cap, &c->len, " ");
    np_fmt_puts(c->buf, c->cap, &c->len, info->owner);
    np_fmt_putc(c->buf, c->cap, &c->len, '\n');
}

size_t net_ports_format_table(char* buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    struct np_fmt_ctx c;
    c.buf = buf;
    c.cap = cap;
    c.len = 0;
    np_fmt_puts(buf, cap, &c.len, "# proto local remote state pid process\n");
    net_ports_sync_tcp();
    net_ports_foreach(np_fmt_one, &c);
    if (c.len < cap) buf[c.len] = 0;
    else if (cap > 0) buf[cap - 1] = 0;
    return c.len < cap ? c.len : cap - 1;
}

static bool np_owner_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

int net_ports_autotest(void) {
    char owner[NET_OWNER_MAX];
    int pid = 0;

    /* dhcpd registers UDP/68 in dhcp_init() */
    if (!net_ports_lookup_owner(NET_PROTO_UDP, 68, owner, sizeof(owner), &pid) ||
        !np_owner_eq(owner, "dhcpd")) {
        return -1;
    }

    const uint16_t test_port = 19090;
    if (net_ports_busy(NET_PROTO_TCP, test_port)) {
        net_ports_close_listen(NET_PROTO_TCP, test_port);
    }

    int fd = socket_create(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -2;
    socket_set_owner(fd, "ports-at", 19090);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = test_port;
    addr.sin_addr = 0;
    if (socket_bind(fd, &addr) != 0) {
        socket_close(fd);
        return -3;
    }
    if (socket_listen(fd, 1) != 0) {
        socket_close(fd);
        return -4;
    }
    if (!net_ports_busy(NET_PROTO_TCP, test_port)) {
        socket_close(fd);
        return -5;
    }
    if (!net_ports_lookup_owner(NET_PROTO_TCP, test_port, owner, sizeof(owner), &pid) ||
        !np_owner_eq(owner, "ports-at") || pid != 19090) {
        socket_close(fd);
        return -6;
    }

    if (net_ports_close_listen(NET_PROTO_TCP, test_port) != 0) return -7;
    if (net_ports_busy(NET_PROTO_TCP, test_port)) return -8;

    int efd = socket_create(AF_INET, SOCK_DGRAM, 0);
    if (efd < 0) return -9;
    socket_set_owner(efd, "ports-ephem", 49152);
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr = 0;
    if (socket_bind(efd, &addr) != 0) {
        socket_close(efd);
        return -10;
    }
    struct sockaddr_in got;
    if (socket_getsockname(efd, &got) != 0 || got.sin_port < 49152) {
        socket_close(efd);
        return -11;
    }
    uint16_t eport = got.sin_port;
    if (!net_ports_busy(NET_PROTO_UDP, eport)) {
        socket_close(efd);
        return -12;
    }
    socket_close(efd);
    if (net_ports_busy(NET_PROTO_UDP, eport)) return -13;

    return 0;
}
