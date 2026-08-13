#include "network_config.h"
#include "dhcp/dhcp.h"
#include "protocols/ip.h"
#include "dns/dns.h"
#include "fs.h"
#include "serial_log.h"
#include <stddef.h>
#include <stdint.h>

static bool netcfg_contains(const char* hay, const char* needle) {
    if (!hay || !needle) return false;
    for (int i = 0; hay[i]; i++) {
        bool ok = true;
        for (int j = 0; needle[j]; j++) {
            if (hay[i + j] != needle[j]) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

static int netcfg_trim_line(char* line) {
    if (!line) return 0;
    int len = 0;
    while (line[len] && line[len] != '\r' && line[len] != '\n') len++;
    line[len] = 0;
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) line[--len] = 0;
    int start = 0;
    while (line[start] == ' ' || line[start] == '\t') start++;
    if (start > 0) {
        int i = 0;
        while (line[start + i]) { line[i] = line[start + i]; i++; }
        line[i] = 0;
        len -= start;
    }
    return len;
}

static bool netcfg_starts(const char* line, const char* prefix) {
    if (!line || !prefix) return false;
    for (int i = 0; prefix[i]; i++) {
        if (line[i] != prefix[i]) return false;
    }
    return true;
}

static int netcfg_parse_ip_field(const char* line, const char* key, uint32_t* out) {
    size_t klen = 0;
    while (key[klen]) klen++;
    if (!netcfg_starts(line, key)) return 0;
    const char* val = line + klen;
    while (*val == ' ' || *val == '\t') val++;
    size_t vlen = 0;
    while (val[vlen] && val[vlen] != ' ' && val[vlen] != '\t') vlen++;
    return ip_parse_address(val, vlen, out);
}

static int netcfg_apply_static(uint32_t ip, uint32_t mask, uint32_t gateway, uint32_t dns) {
    if (ip == 0) return -1;
    if (mask == 0) mask = 0xFFFFFF00;
    network_apply_config(ip, mask, gateway, dns);
    log_ip(LOG_INFO, "netcfg", "static applied", ip);
    return 0;
}

static int netcfg_apply_dhcp(void) {
    log_msg(LOG_DBG, "netcfg", "starting DHCP");
    if (dhcp_acquire_quiet() != 0) {
        log_msg(LOG_ERR, "netcfg", "DHCP failed");
        return -1;
    }
    return 0;
}

static int netcfg_load_file(const char* path, char* buf, size_t buf_size);

static int netcfg_parse_resolv_dns(uint32_t* dns) {
    if (!dns) return -1;
    char buf[160];
    if (netcfg_load_file(NETWORK_RESOLV_PATH, buf, sizeof(buf)) < 0) return -1;
    size_t len = 0;
    while (len < sizeof(buf) && buf[len]) len++;

    char line[64];
    size_t pos = 0;
    while (pos < len) {
        size_t li = 0;
        while (pos < len && buf[pos] != '\n' && li + 1 < sizeof(line)) line[li++] = buf[pos++];
        if (pos < len && buf[pos] == '\n') pos++;
        line[li] = 0;
        if (netcfg_trim_line(line) == 0) continue;
        if (line[0] == '#') continue;
        if (netcfg_starts(line, "nameserver ")) {
            const char* val = line + 11;
            while (*val == ' ' || *val == '\t') val++;
            size_t vlen = 0;
            while (val[vlen] && val[vlen] != ' ' && val[vlen] != '\t') vlen++;
            if (ip_parse_address(val, vlen, dns) == 0) return 0;
        }
    }
    return -1;
}

static int netcfg_parse_buffer(const char* text, size_t len) {
    enum { MODE_NONE, MODE_DHCP, MODE_STATIC } mode = MODE_NONE;
    uint32_t ip = 0, mask = 0, gateway = 0, dns = 0;

    char line[128];
    size_t pos = 0;
    while (pos < len) {
        size_t li = 0;
        while (pos < len && text[pos] != '\n' && li + 1 < sizeof(line)) line[li++] = text[pos++];
        if (pos < len && text[pos] == '\n') pos++;
        line[li] = 0;
        if (netcfg_trim_line(line) == 0) continue;
        if (line[0] == '#') continue;

        if (netcfg_starts(line, "iface ") && netcfg_contains(line, " inet dhcp")) {
            mode = MODE_DHCP;
            continue;
        }
        if (netcfg_starts(line, "iface ") && netcfg_contains(line, " inet static")) {
            mode = MODE_STATIC;
            continue;
        }
        if (netcfg_starts(line, "METHOD=dhcp")) { mode = MODE_DHCP; continue; }
        if (netcfg_starts(line, "METHOD=static")) { mode = MODE_STATIC; continue; }

        if (netcfg_parse_ip_field(line, "address ", &ip) != 0) netcfg_parse_ip_field(line, "IP=", &ip);
        if (netcfg_parse_ip_field(line, "netmask ", &mask) != 0) netcfg_parse_ip_field(line, "NETMASK=", &mask);
        if (netcfg_parse_ip_field(line, "gateway ", &gateway) != 0) netcfg_parse_ip_field(line, "GATEWAY=", &gateway);
        if (netcfg_parse_ip_field(line, "dns ", &dns) != 0) netcfg_parse_ip_field(line, "DNS=", &dns);
        if (netcfg_parse_ip_field(line, "dns-nameservers ", &dns) != 0) {
            netcfg_parse_ip_field(line, "dns-nameserver ", &dns);
        }
    }

    if (mode == MODE_STATIC && ip != 0 && mask == 0) mask = 0xFFFFFF00;
    if (dns == 0) {
        if (netcfg_parse_resolv_dns(&dns) == 0) {
            log_ip(LOG_DBG, "netcfg", "dns from resolv", dns);
        }
    }

    if (mode == MODE_DHCP) return netcfg_apply_dhcp();
    if (mode == MODE_STATIC) {
        int rc = netcfg_apply_static(ip, mask, gateway, dns);
        if (rc == 0) {
            log_ip(LOG_INFO, "netcfg", "parsed static ip", ip);
            log_fmt3(LOG_INFO, "netcfg", "parsed static", "gw", gateway, "dns", dns, "mask", mask);
        }
        return rc;
    }
    return -1;
}

static int netcfg_load_file(const char* path, char* buf, size_t buf_size) {
    uint32_t size = 0;
    if (fs_open(path, &size) != 0) return -1;
    if (size == 0 || size >= buf_size) return -1;
    if (fs_read(path, buf, (size_t)size) < 0) return -1;
    buf[size] = 0;
    return (int)size;
}

static int netcfg_apply_from_disk(void) {
    char buf[768];
    if (netcfg_load_file(NETWORK_CONFIG_PATH, buf, sizeof(buf)) < 0) return -1;
    size_t len = 0;
    while (len < sizeof(buf) && buf[len]) len++;
    return netcfg_parse_buffer(buf, len);
}

static void netcfg_append(char* buf, int* pos, int max, const char* s) {
    while (*s && *pos < max - 1) buf[(*pos)++] = *s++;
}

static void netcfg_append_dec(char* buf, int* pos, int max, uint32_t v) {
    char tmp[12];
    int t = 0;
    if (v == 0) {
        netcfg_append(buf, pos, max, "0");
        return;
    }
    while (v > 0 && t < 11) {
        tmp[t++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (t > 0) {
        char ch[2] = { tmp[--t], 0 };
        netcfg_append(buf, pos, max, ch);
    }
}

static void netcfg_append_ip_line(char* buf, int* pos, int max, const char* key, uint32_t ip) {
    char ipstr[20];
    ip_format_address(ip, ipstr, sizeof(ipstr));
    netcfg_append(buf, pos, max, "    ");
    netcfg_append(buf, pos, max, key);
    netcfg_append(buf, pos, max, ipstr);
    netcfg_append(buf, pos, max, "\n");
}

static int netcfg_gather_runtime(uint32_t* ip, uint32_t* mask, uint32_t* gw, uint32_t* dns,
                                 uint32_t* network, uint32_t* broadcast,
                                 uint32_t* lease, uint32_t* dhcp_srv, bool* from_dhcp) {
    if (!ip || !mask || !gw || !dns) return -1;
    *from_dhcp = false;
    *lease = 0;
    *dhcp_srv = 0;

    struct dhcp_config dc;
    if (dhcp_get_config(&dc) == 0) {
        *ip = dc.ip_address;
        *mask = dc.subnet_mask;
        *gw = dc.gateway;
        *dns = dc.dns_server;
        *lease = dc.lease_time;
        *dhcp_srv = dc.server_ip;
        *from_dhcp = true;
    } else {
        *ip = ip_get_our_ip();
        *mask = ip_get_subnet_mask();
        *gw = ip_get_gateway();
        *dns = dns_get_server();
    }

    if (*ip == 0) return -1;
    if (*mask == 0) *mask = 0xFFFFFF00;
    if (*dns == 0) *dns = 0x0A000203;
    if (network) *network = *ip & *mask;
    if (broadcast) *broadcast = (*network) | (~(*mask));
    return 0;
}

static void netcfg_write_resolv(uint32_t dns) {
    if (dns == 0) {
        dns = 0x0A000203;
    }
    fs_create_dir("/etc");
    char resolv[160];
    char ipstr[20];
    int rp = 0;
    netcfg_append(resolv, &rp, (int)sizeof(resolv), "# Generated by My OS\n");
    netcfg_append(resolv, &rp, (int)sizeof(resolv), "nameserver ");
    ip_format_address(dns, ipstr, sizeof(ipstr));
    netcfg_append(resolv, &rp, (int)sizeof(resolv), ipstr);
    netcfg_append(resolv, &rp, (int)sizeof(resolv), "\n");
    netcfg_append(resolv, &rp, (int)sizeof(resolv), "search local\n");
    netcfg_append(resolv, &rp, (int)sizeof(resolv), "options timeout:2 attempts:3\n");
    resolv[rp] = 0;
    if (rp <= 0) {
        log_msg(LOG_ERR, "netcfg", "resolv empty");
        return;
    }

    fs_delete(NETWORK_RESOLV_PATH);
    if (fs_write(NETWORK_RESOLV_PATH, resolv, (size_t)rp) != 0) {
        log_msg(LOG_ERR, "netcfg", "resolv write failed (fs not ready?)");
        return;
    }
    log_fmt3(LOG_INFO, "netcfg", "resolv written", "bytes", (uint32_t)rp, "dns", dns, "ok", 1u);
}

static int netcfg_write_interfaces(void) {
    uint32_t ip = 0, mask = 0, gw = 0, dns = 0;
    uint32_t network = 0, broadcast = 0, lease = 0, dhcp_srv = 0;
    bool from_dhcp = false;
    if (netcfg_gather_runtime(&ip, &mask, &gw, &dns, &network, &broadcast, &lease, &dhcp_srv,
                              &from_dhcp) != 0) {
        return -1;
    }

    char buf[640];
    int pos = 0;
    netcfg_append(buf, &pos, (int)sizeof(buf), "# /etc/network/interfaces\n");
    netcfg_append(buf, &pos, (int)sizeof(buf), "# Debian-style network config (My OS)\n");
    if (from_dhcp) {
        netcfg_append(buf, &pos, (int)sizeof(buf), "# Values captured from DHCP lease\n");
    }
    netcfg_append(buf, &pos, (int)sizeof(buf), "\n");
    netcfg_append(buf, &pos, (int)sizeof(buf), "auto lo\n");
    netcfg_append(buf, &pos, (int)sizeof(buf), "iface lo inet loopback\n");
    netcfg_append(buf, &pos, (int)sizeof(buf), "\n");
    netcfg_append(buf, &pos, (int)sizeof(buf), "auto eth0\n");
    netcfg_append(buf, &pos, (int)sizeof(buf), "iface eth0 inet static\n");
    netcfg_append_ip_line(buf, &pos, (int)sizeof(buf), "address ", ip);
    netcfg_append_ip_line(buf, &pos, (int)sizeof(buf), "netmask ", mask);
    netcfg_append_ip_line(buf, &pos, (int)sizeof(buf), "network ", network);
    netcfg_append_ip_line(buf, &pos, (int)sizeof(buf), "broadcast ", broadcast);
    if (gw) {
        netcfg_append_ip_line(buf, &pos, (int)sizeof(buf), "gateway ", gw);
    }
    netcfg_append_ip_line(buf, &pos, (int)sizeof(buf), "dns-nameservers ", dns);
    netcfg_append(buf, &pos, (int)sizeof(buf), "    mtu 1500\n");
    netcfg_append(buf, &pos, (int)sizeof(buf), "    hostname knitos\n");
    if (from_dhcp && lease > 0) {
        char ipstr[20];
        netcfg_append(buf, &pos, (int)sizeof(buf), "\n# DHCP metadata\n");
        if (dhcp_srv) {
            ip_format_address(dhcp_srv, ipstr, sizeof(ipstr));
            netcfg_append(buf, &pos, (int)sizeof(buf), "# dhcp-server ");
            netcfg_append(buf, &pos, (int)sizeof(buf), ipstr);
            netcfg_append(buf, &pos, (int)sizeof(buf), "\n");
        }
        netcfg_append(buf, &pos, (int)sizeof(buf), "# lease-time ");
        netcfg_append_dec(buf, &pos, (int)sizeof(buf), lease);
        netcfg_append(buf, &pos, (int)sizeof(buf), "\n");
    }
    buf[pos] = 0;
    if (fs_write(NETWORK_CONFIG_PATH, buf, (size_t)pos) != 0) return -1;
    log_ip(LOG_INFO, "netcfg", "interfaces saved", ip);
    return 0;
}

int network_config_reload(void) {
    int rc = netcfg_apply_from_disk();
    if (rc == 0) {
        netcfg_write_resolv(dns_get_server());
    }
    return rc;
}

int network_config_acquire_dhcp(void) {
    fs_create_dir("/etc");
    fs_create_dir("/etc/network");
    const char* dhcp_cfg =
        "# autotest / manual DHCP\n"
        "auto eth0\n"
        "iface eth0 inet dhcp\n";
    size_t dlen = 0;
    while (dhcp_cfg[dlen]) dlen++;
    if (fs_write(NETWORK_CONFIG_PATH, dhcp_cfg, dlen) != 0) {
        return -1;
    }
    log_msg(LOG_INFO, "netcfg", "dhcp mode written");
    dhcp_release();
    if (dhcp_acquire() != 0) {
        log_msg(LOG_ERR, "netcfg", "dhcp acquire failed");
        return -1;
    }
    network_config_save();
    return 0;
}

int network_config_apply_boot(void) {
    if (netcfg_apply_from_disk() == 0) {
        log_ip(LOG_INFO, "netcfg", "loaded from disk ip", ip_get_our_ip());
        log_fmt3(LOG_INFO, "netcfg", "runtime", "gw", ip_get_gateway(),
                 "dns", dns_get_server(), "mask", ip_get_subnet_mask());
        /* Без disk write на boot: IRQ ещё off, AHCI write через WSL вешает boot. */
        log_msg(LOG_INFO, "netcfg", "static boot ok");
        return 0;
    }
    log_msg(LOG_DBG, "netcfg", "no config, DHCP default");
    int rc = netcfg_apply_dhcp();
    if (rc == 0) {
        /* Не писать resolv/interfaces здесь — только runtime IP. */
        log_msg(LOG_INFO, "netcfg", "DHCP boot ok");
    }
    return rc;
}

int network_config_save(void) {
    log_msg(LOG_DBG, "netcfg", "save begin");
    fs_create_dir("/etc");
    fs_create_dir("/etc/network");

    if (netcfg_write_interfaces() != 0) {
        log_msg(LOG_ERR, "netcfg", "interfaces write failed");
        return -1;
    }

    uint32_t dns = dns_get_server();
    if (dns == 0) dns = 0x0A000203;
    netcfg_write_resolv(dns);

    log_msg(LOG_INFO, "netcfg", "saved to disk");
    return 0;
}
