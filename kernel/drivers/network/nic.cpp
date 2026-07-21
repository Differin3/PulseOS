#include "nic.h"
#include "drivers/pci/pci.h"
#include "drivers/video/terminal.h"
#include "driver_manager.h"
#include "protocols/ethernet.h"
#include "protocols/arp.h"
#include "protocols/ip.h"
#include "protocols/udp.h"
#include "protocols/tcp.h"
#include "protocols/icmp.h"
#include "dns/dns.h"
#include "dhcp/dhcp.h"
#include "socket.h"
#include "drivers/rtl8139/rtl8139.h"
#include "drivers/pcnet/pcnet.h"
#include "serial_log.h"
#include <stddef.h>

static inline uint16_t ntohs(uint16_t netshort) { return ((netshort & 0xFF) << 8) | ((netshort >> 8) & 0xFF); }

static struct nic_device nic_dev = { 0, 0, {0,0,0,0,0,0}, 0, false, false, nullptr, nullptr, 0, NIC_HW_NONE };
static bool nic_initialized = false;

extern void udp_handle_packet(uint32_t src_ip, uint32_t dest_ip,
                              const void* ip_payload, size_t payload_size);
extern void dhcp_handle_packet(uint32_t src_ip, uint16_t src_port,
                               const void* udp_payload, size_t payload_size);
extern void tcp_handle_packet(uint32_t src_ip, uint32_t dest_ip, const void* ip_payload, size_t payload_size);
extern void tcp_process_timers();
extern void tcp_init();
extern void dns_init();
extern void arp_init();
extern void dns_handle_request(uint32_t src_ip, uint16_t src_port, const void* udp_payload, size_t payload_size);
extern void dns_handle_response(const void* udp_payload, size_t payload_size);
extern int udp_parse_header(const void* packet, size_t packet_size,
                           struct udp_header* header, const void** payload, size_t* payload_size);

int nic_driver_read(void* device_data, void* buffer, size_t size, uint32_t offset); // one line
int nic_driver_write(void* device_data, const void* buffer, size_t size, uint32_t offset); // one line
int nic_driver_ioctl(void* device_data, uint32_t cmd, void* arg); // one line

static void nic_init_network_stack() {
    arp_init();
    dns_init();
    tcp_init();
    socket_init();
}

static int nic_register_driver() {
    struct driver nic_driver;
    nic_driver.name[0] = 'n'; nic_driver.name[1] = 'i'; nic_driver.name[2] = 'c'; nic_driver.name[3] = '0'; nic_driver.name[4] = 0;
    nic_driver.type = DRIVER_NETWORK;
    nic_driver.device_id = 0;
    nic_driver.device_data = &nic_dev;
    nic_driver.initialized = true;
    nic_driver.active = true;
    nic_driver.ops.init = 0;
    nic_driver.ops.read = nic_driver_read;
    nic_driver.ops.write = nic_driver_write;
    nic_driver.ops.ioctl = nic_driver_ioctl;
    nic_driver.ops.cleanup = 0;
    return driver_register(&nic_driver);
}

int nic_driver_read(void* device_data, void* buffer, size_t size, uint32_t offset) { (void)device_data; (void)offset; return nic_receive_packet(buffer, size); }
int nic_driver_write(void* device_data, const void* buffer, size_t size, uint32_t offset) { (void)device_data; (void)offset; return nic_send_packet(buffer, size); }

int nic_driver_ioctl(void* device_data, uint32_t cmd, void* arg) {
    (void)device_data; // одна точка входа управления NIC
    if (cmd == NIC_IOCTL_GET_MAC && arg) { uint8_t* mac = (uint8_t*)arg; nic_get_mac(mac); return 0; }
    if (cmd == NIC_IOCTL_SET_IP && arg) { uint32_t* ip_val = (uint32_t*)arg; ip_set_our_ip(*ip_val); return 0; }
    if (cmd == NIC_IOCTL_GET_IP && arg) { uint32_t* ip_val = (uint32_t*)arg; *ip_val = ip_get_our_ip(); return 0; }
    if (cmd == NIC_IOCTL_UDP_SEND && arg) { struct { uint32_t dest_ip; uint16_t src_port; uint16_t dest_port; void* data; size_t len; }* udp_args = (typeof(udp_args))arg; return udp_send(udp_args->dest_ip, udp_args->src_port, udp_args->dest_port, udp_args->data, udp_args->len); }
    return -1;
}

int nic_init() {
    if (nic_initialized) return 0; // уже инициализирован
    struct pci_device dev;
    // сначала ищем RTL8139
    if (find_rtl8139(&dev) == 0) {
        if (rtl8139_init_with_pci(&dev, &nic_dev) == 0) {
            nic_initialized = true;
            if (nic_register_driver() == 0) {
                nic_init_network_stack();
                return 0;
            }
        }
    }
    // затем пытаемся AMD PCnet
    for (int bus = 0; bus < 16; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            for (uint8_t function = 0; function < 8; function++) {
                uint32_t vd = pci_read_config((uint8_t)bus, device, function, 0x00);
                uint16_t vendor_id = vd & 0xFFFF; uint16_t device_id = (vd >> 16) & 0xFFFF;
                if (vendor_id == 0x1022 && device_id == 0x2000) {
                    dev.vendor_id = vendor_id; dev.device_id = device_id; dev.bus = (uint8_t)bus; dev.device = device; dev.function = function;
                    for (int i = 0; i < 6; i++) dev.base_address[i] = pci_read_config((uint8_t)bus, device, function, 0x10 + i * 4);
                    if (pcnet_init_with_pci(&dev, &nic_dev) == 0) {
                        nic_initialized = true;
                        if (nic_register_driver() == 0) {
                            nic_init_network_stack();
                            return 0;
                        }
                    }
                }
            }
        }
    }
    return -1;
}

int nic_init_with_device(const struct pci_device* dev) {
    if (nic_initialized) return 0;
    if (!dev) return -1;
    extern void terminal_writestring(const char* str);
    if (dev->vendor_id == 0x10EC && dev->device_id == 0x8139) {
        if (rtl8139_init_with_pci(dev, &nic_dev) == 0) {
            nic_initialized = true;
            if (nic_register_driver() == 0) {
                nic_init_network_stack();
                return 0;
            }
        }
    }
    if (dev->vendor_id == 0x1022 && dev->device_id == 0x2000) {
        if (pcnet_init_with_pci(dev, &nic_dev) == 0) {
            nic_initialized = true;
            if (nic_register_driver() == 0) {
                nic_init_network_stack();
                return 0;
            }
        }
    }
    return -1;
}

int nic_send_packet(const void* data, size_t len) {
    if (!nic_initialized || !nic_dev.active) {
        extern void terminal_writestring(const char* str);
        terminal_writestring("\n[NIC] ERROR: NIC not initialized or not active");
        log_msg(LOG_ERR, "nic", "not initialized or not active");
        return -1;
    }
    if (len > ETH_MAX_PACKET_SIZE) {
        extern void terminal_writestring(const char* str);
        terminal_writestring("\n[NIC] ERROR: Packet too large");
        log_msg(LOG_ERR, "nic", "packet too large");
        return -1;
    }
    int result = -1;
    if (nic_dev.hw_type == NIC_HW_PCNET) result = pcnet_send_packet(&nic_dev, data, len);
    else if (nic_dev.hw_type == NIC_HW_RTL8139) result = rtl8139_send_packet(&nic_dev, data, len);
    if (result < 0) {
        extern void terminal_writestring(const char* str);
        terminal_writestring("\n[NIC] ERROR: Hardware send failed");
        log_fmt3(LOG_ERR, "nic", "hardware send failed", "len", (uint32_t)len, "hw", (uint32_t)nic_dev.hw_type, "ret", (uint32_t)result);
        return -1;
    }
    log_fmt3(LOG_DBG, "nic", "sent", "len", (uint32_t)len, "hw", (uint32_t)nic_dev.hw_type, "ok", (uint32_t)result);
    return 0;
}

int nic_receive_packet(void* buffer, size_t max_len) {
    if (!nic_initialized || !nic_dev.active) return 0; // нет данных
    if (nic_dev.hw_type == NIC_HW_PCNET) return pcnet_receive_packet(&nic_dev, buffer, max_len);
    if (nic_dev.hw_type == NIC_HW_RTL8139) return rtl8139_receive_packet(&nic_dev, buffer, max_len);
    return 0;
}

void nic_get_mac(uint8_t* mac) { if (!mac) return; for (int i = 0; i < 6; i++) mac[i] = nic_dev.mac_address[i]; }

bool nic_has_packet() {
    if (!nic_initialized || !nic_dev.active) return false;
    if (nic_dev.hw_type == NIC_HW_PCNET) return pcnet_has_packet(&nic_dev);
    if (nic_dev.hw_type == NIC_HW_RTL8139) return rtl8139_has_packet(&nic_dev);
    return false;
}

void nic_debug_rx_regs(uint8_t* cmd, uint16_t* isr, uint16_t* capr, uint16_t* cbr) {
    if (nic_dev.hw_type == NIC_HW_RTL8139) {
        rtl8139_rx_regs(&nic_dev, cmd, isr, capr, cbr);
        return;
    }
    if (cmd) *cmd = 0;
    if (isr) *isr = 0;
    if (capr) *capr = 0;
    if (cbr) *cbr = 0;
}

void nic_process_packets() {
    if (!nic_initialized || !nic_dev.active) return;
    uint8_t packet_buffer[ETH_MAX_PACKET_SIZE];
    int guard = 0;
    while (nic_has_packet() && guard < 32) {
        guard++;
        int packet_len = nic_receive_packet(packet_buffer, sizeof(packet_buffer));
        if (packet_len <= 0) {
            if (nic_has_packet()) {
                log_msg(LOG_DBG, "nic", "rx pending but len=0");
            }
            continue;
        }

        struct ethernet_header eth_header; const void* payload; size_t payload_size;
        if (ethernet_parse_header(packet_buffer, packet_len, &eth_header, &payload, &payload_size) != 0) {
            log_msg(LOG_ERR, "nic", "eth parse fail");
            continue;
        }
        uint16_t protocol_type = ntohs(eth_header.type);
        log_fmt3(LOG_INFO, "nic", "eth", "type", (uint32_t)protocol_type, "plen", (uint32_t)payload_size, "len", (uint32_t)packet_len);

        if (protocol_type == ETH_TYPE_ARP) {
            if (payload_size >= sizeof(struct arp_packet)) arp_handle_packet((const struct arp_packet*)payload, payload_size);
        } else if (protocol_type == ETH_TYPE_IPV4) {
            struct ip_header ip_hdr; const void* ip_payload; size_t ip_payload_size;
            if (ip_parse_header(payload, payload_size, &ip_hdr, &ip_payload, &ip_payload_size) != 0) {
                log_msg(LOG_ERR, "nic", "ip parse fail");
                continue;
            }
            if (ip_hdr.src_ip != 0 && ip_hdr.src_ip != 0xFFFFFFFF) {
                arp_add_entry(ip_hdr.src_ip, eth_header.src_mac);
            }
            if (ip_is_local_dest(ip_hdr.dest_ip)) {
                log_fmt3(LOG_DBG, "nic", "ipv4", "proto", (uint32_t)ip_hdr.protocol, "len", (uint32_t)ip_payload_size, "dest", ip_hdr.dest_ip);
                if (ip_hdr.protocol == IP_PROTOCOL_UDP) {
                    udp_handle_packet(ip_hdr.src_ip, ip_hdr.dest_ip, ip_payload, ip_payload_size);
                    struct udp_header udp_hdr; const void* udp_payload; size_t udp_payload_size;
                    if (udp_parse_header(ip_payload, ip_payload_size, &udp_hdr, &udp_payload, &udp_payload_size) == 0) {
                        uint16_t dest_port = ((udp_hdr.dest_port & 0xFF) << 8) | ((udp_hdr.dest_port >> 8) & 0xFF);
                        uint16_t src_port = ((udp_hdr.src_port & 0xFF) << 8) | ((udp_hdr.src_port >> 8) & 0xFF);
                        if (dest_port == 68) {
                            log_fmt3(LOG_DBG, "nic", "udp dhcp", "sport", (uint32_t)src_port, "dport", (uint32_t)dest_port, "len", (uint32_t)udp_payload_size);
                        }
                        if (dest_port == DNS_PORT) {
                            dns_handle_request(ip_hdr.src_ip, src_port, udp_payload, udp_payload_size);
                        } else if (dest_port == DNS_CLIENT_PORT) {
                            dns_handle_response(udp_payload, udp_payload_size);
                        } else if (dest_port == 68) {
                            dhcp_handle_packet(ip_hdr.src_ip, src_port, udp_payload, udp_payload_size);
                        }
                    }
                } else if (ip_hdr.protocol == IP_PROTOCOL_TCP) {
                    log_fmt3(LOG_INFO, "nic", "tcp rx", "len", (uint32_t)ip_payload_size,
                             "src", ip_hdr.src_ip, "dest", ip_hdr.dest_ip);
                    tcp_handle_packet(ip_hdr.src_ip, ip_hdr.dest_ip, ip_payload, ip_payload_size);
                } else if (ip_hdr.protocol == IP_PROTOCOL_ICMP) {
                    icmp_handle_packet(ip_hdr.src_ip, ip_payload, ip_payload_size);
                }
            } else {
                log_ip(LOG_INFO, "nic", "drop dest", ip_hdr.dest_ip);
            }
        }
    }
    if (guard >= 32 && nic_has_packet()) {
        log_msg(LOG_ERR, "nic", "rx loop guard hit");
    }
}
