#include "dhcp.h"
#include "../protocols/udp.h"
#include "../protocols/ip.h"
#include "../nic.h"
#include "../protocols/ethernet.h"
#include "../protocols/arp.h"
#include "../dns/dns.h"
#include "../protocols/tcp_connection.h"
#include "../core/net_ports.h"
#include "drivers/timer/pit.h"
#include "serial_log.h"
#include "sched/task.h"

// Объявления функций из протоколов
extern void ip_set_our_ip(uint32_t ip);
extern void ip_set_subnet_mask(uint32_t mask);
extern void ip_set_gateway(uint32_t gateway);
extern void dns_set_server(uint32_t dns_ip);
#include <stddef.h>

extern void terminal_writestring(const char* str);

// Вспомогательные функции для byte order
static inline uint16_t htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort >> 8) & 0xFF);
}

static inline uint32_t htonl(uint32_t hostlong) {
    return ((hostlong & 0xFF) << 24) | ((hostlong & 0xFF00) << 8) |
           ((hostlong & 0xFF0000) >> 8) | ((hostlong & 0xFF000000) >> 24);
}

static inline uint16_t ntohs(uint16_t netshort) {
    return ((netshort & 0xFF) << 8) | ((netshort >> 8) & 0xFF);
}

static inline uint32_t ntohl(uint32_t netlong) {
    return ((netlong & 0xFF) << 24) | ((netlong & 0xFF00) << 8) |
           ((netlong & 0xFF0000) >> 8) | ((netlong & 0xFF000000) >> 24);
}

// DHCP состояние
static enum dhcp_state dhcp_current_state = DHCP_STATE_IDLE;
static struct dhcp_config dhcp_config = {0};
static uint32_t dhcp_xid = 0; // Transaction ID
static uint32_t dhcp_server_ip = 0; // IP адрес DHCP сервера
static uint32_t dhcp_offered_ip = 0; // Предложенный IP адрес
static uint32_t dhcp_bound_at = 0;
static bool dhcp_renew_in_progress = false;
static uint32_t dhcp_renew_sent_at = 0;
static bool dhcp_verbose = false;
static volatile int g_dhcpd_pid = -1;

static void dhcp_print(const char* msg) {
    if (dhcp_verbose) {
        terminal_writestring(msg);
    }
}

#define DHCP_RENEW_TIMEOUT_TICKS 300

static uint32_t dhcp_generate_xid() {
    uint8_t mac[6];
    nic_get_mac(mac);
    uint32_t xid = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                   ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
    xid ^= tcp_get_time() * 0x9E3779B1u;
    return xid ? xid : 0xA5A5A5A5u;
}

// Добавить DHCP опцию в буфер
static int dhcp_add_option(uint8_t* buffer, size_t buffer_size, size_t* pos,
                          uint8_t code, const void* data, uint8_t length) {
    if (!buffer || !pos || *pos + 2 + length > buffer_size) return -1;
    
    buffer[(*pos)++] = code;
    if (code != DHCP_OPTION_PAD && code != DHCP_OPTION_END) {
        buffer[(*pos)++] = length;
        for (uint8_t i = 0; i < length; i++) {
            buffer[(*pos)++] = ((const uint8_t*)data)[i];
        }
    }
    return 0;
}

// Найти DHCP опцию в пакете
static const uint8_t* dhcp_find_option(const uint8_t* options, size_t options_size,
                                      uint8_t code, uint8_t* length) {
    if (!options || !length) return NULL;
    
    size_t pos = 0;
    while (pos < options_size) {
        uint8_t opt_code = options[pos];
        if (opt_code == DHCP_OPTION_END) break;
        if (opt_code == DHCP_OPTION_PAD) {
            pos++;
            continue;
        }
        
        if (pos + 1 >= options_size) break;
        uint8_t opt_len = options[pos + 1];
        if (pos + 2 + opt_len > options_size) break;
        
        if (opt_code == code) {
            *length = opt_len;
            return &options[pos + 2];
        }
        
        pos += 2 + opt_len;
    }
    
    return NULL;
}

// Отправить DHCP DISCOVER
static int dhcp_send_discover() {
    uint8_t our_mac[6];
    nic_get_mac(our_mac);
    
    // Генерируем случайный transaction ID
    dhcp_xid = dhcp_generate_xid();
    
    // Создаем DHCP заголовок
    struct dhcp_header dhcp_hdr = {0};
    dhcp_hdr.op = 1; // BOOTREQUEST
    dhcp_hdr.htype = 1; // Ethernet
    dhcp_hdr.hlen = 6;
    dhcp_hdr.xid = htonl(dhcp_xid);
    dhcp_hdr.flags = htons(0x8000); // Broadcast flag
    for (int i = 0; i < 6; i++) dhcp_hdr.chaddr[i] = our_mac[i];
    dhcp_hdr.magic = htonl(DHCP_MAGIC_COOKIE);
    
    // Создаем опции
    uint8_t dhcp_buffer[512];
    size_t pos = 0;
    
    // Копируем заголовок
    for (size_t i = 0; i < sizeof(struct dhcp_header); i++) {
        dhcp_buffer[pos++] = ((uint8_t*)&dhcp_hdr)[i];
    }
    
    // Добавляем опции
    uint8_t msg_type = DHCP_MESSAGE_TYPE_DISCOVER;
    dhcp_add_option(dhcp_buffer, sizeof(dhcp_buffer), &pos, DHCP_OPTION_MESSAGE_TYPE, &msg_type, 1);
    
    uint8_t param_list[] = {DHCP_OPTION_SUBNET_MASK, DHCP_OPTION_ROUTER, DHCP_OPTION_DNS_SERVER};
    dhcp_add_option(dhcp_buffer, sizeof(dhcp_buffer), &pos, DHCP_OPTION_PARAMETER_REQUEST_LIST, param_list, 3);
    
    dhcp_add_option(dhcp_buffer, sizeof(dhcp_buffer), &pos, DHCP_OPTION_END, NULL, 0);
    
    // Отправляем через UDP на broadcast (255.255.255.255:67)
    uint32_t broadcast_ip = 0xFFFFFFFF;
    if (udp_send(broadcast_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, dhcp_buffer, pos) != 0) {
        log_fmt3(LOG_ERR, "dhcp", "DISCOVER send failed", "xid", dhcp_xid, "bytes", (uint32_t)pos, "port", (uint32_t)DHCP_SERVER_PORT);
        dhcp_print("\n[DHCP] ERROR: Failed to send DISCOVER via UDP");
        return -1;
    }

    log_fmt3(dhcp_verbose ? LOG_INFO : LOG_DBG, "dhcp", "DISCOVER sent", "xid", dhcp_xid, "bytes", (uint32_t)pos, "port", (uint32_t)DHCP_SERVER_PORT);
    return 0;
}

// Отправить DHCP REQUEST (renew: ciaddr = наш IP)
static int dhcp_send_request(uint32_t requested_ip, uint32_t server_ip) {
    uint8_t our_mac[6];
    nic_get_mac(our_mac);
    
    // Создаем DHCP заголовок
    struct dhcp_header dhcp_hdr = {0};
    dhcp_hdr.op = 1; // BOOTREQUEST
    dhcp_hdr.htype = 1;
    dhcp_hdr.hlen = 6;
    dhcp_hdr.xid = htonl(dhcp_xid);
    dhcp_hdr.flags = htons(0x8000); // Broadcast
    dhcp_hdr.ciaddr = (dhcp_config.valid && dhcp_config.ip_address != 0)
        ? htonl(dhcp_config.ip_address) : 0;
    for (int i = 0; i < 6; i++) dhcp_hdr.chaddr[i] = our_mac[i];
    dhcp_hdr.magic = htonl(DHCP_MAGIC_COOKIE);
    
    uint8_t dhcp_buffer[512];
    size_t pos = 0;
    
    // Копируем заголовок
    for (size_t i = 0; i < sizeof(struct dhcp_header); i++) {
        dhcp_buffer[pos++] = ((uint8_t*)&dhcp_hdr)[i];
    }
    
    // Добавляем опции
    uint8_t msg_type = DHCP_MESSAGE_TYPE_REQUEST;
    dhcp_add_option(dhcp_buffer, sizeof(dhcp_buffer), &pos, DHCP_OPTION_MESSAGE_TYPE, &msg_type, 1);
    
    uint32_t req_ip = htonl(requested_ip);
    dhcp_add_option(dhcp_buffer, sizeof(dhcp_buffer), &pos, DHCP_OPTION_REQUESTED_IP, &req_ip, 4);
    
    uint32_t srv_ip = htonl(server_ip);
    dhcp_add_option(dhcp_buffer, sizeof(dhcp_buffer), &pos, DHCP_OPTION_SERVER_IDENTIFIER, &srv_ip, 4);
    
    uint8_t param_list[] = {DHCP_OPTION_SUBNET_MASK, DHCP_OPTION_ROUTER, DHCP_OPTION_DNS_SERVER};
    dhcp_add_option(dhcp_buffer, sizeof(dhcp_buffer), &pos, DHCP_OPTION_PARAMETER_REQUEST_LIST, param_list, 3);
    
    dhcp_add_option(dhcp_buffer, sizeof(dhcp_buffer), &pos, DHCP_OPTION_END, NULL, 0);
    
    // Отправляем broadcast
    uint32_t broadcast_ip = 0xFFFFFFFF;
    if (udp_send(broadcast_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, dhcp_buffer, pos) != 0) {
        dhcp_print("\n[DHCP] ERROR: Failed to send REQUEST via UDP");
        return -1;
    }
    
    if (!dhcp_renew_in_progress) {
        dhcp_print("\n[DHCP] REQUEST sent");
    }
    return 0;
}

// Отправить DHCP RELEASE
static int dhcp_send_release() {
    if (!dhcp_config.valid || dhcp_config.ip_address == 0) {
        return 0;
    }

    uint8_t our_mac[6];
    nic_get_mac(our_mac);

    struct dhcp_header dhcp_hdr = {0};
    dhcp_hdr.op = 1;
    dhcp_hdr.htype = 1;
    dhcp_hdr.hlen = 6;
    dhcp_hdr.xid = htonl(dhcp_xid);
    dhcp_hdr.flags = htons(0x8000);
    dhcp_hdr.ciaddr = htonl(dhcp_config.ip_address);
    for (int i = 0; i < 6; i++) dhcp_hdr.chaddr[i] = our_mac[i];
    dhcp_hdr.magic = htonl(DHCP_MAGIC_COOKIE);

    uint8_t dhcp_buffer[512];
    size_t pos = 0;

    for (size_t i = 0; i < sizeof(struct dhcp_header); i++) {
        dhcp_buffer[pos++] = ((uint8_t*)&dhcp_hdr)[i];
    }

    uint8_t msg_type = DHCP_MESSAGE_TYPE_RELEASE;
    dhcp_add_option(dhcp_buffer, sizeof(dhcp_buffer), &pos, DHCP_OPTION_MESSAGE_TYPE, &msg_type, 1);

    uint32_t srv_ip = htonl(dhcp_config.server_ip);
    if (dhcp_config.server_ip != 0) {
        dhcp_add_option(dhcp_buffer, sizeof(dhcp_buffer), &pos, DHCP_OPTION_SERVER_IDENTIFIER, &srv_ip, 4);
    }

    dhcp_add_option(dhcp_buffer, sizeof(dhcp_buffer), &pos, DHCP_OPTION_END, NULL, 0);

    uint32_t broadcast_ip = 0xFFFFFFFF;
    if (udp_send(broadcast_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, dhcp_buffer, pos) != 0) {
        return -1;
    }

    terminal_writestring("\n[DHCP] RELEASE sent");
    return 0;
}

// Парсить DHCP пакет и извлечь опции
static int dhcp_parse_packet(const void* packet, size_t packet_size,
                            struct dhcp_header* header, const uint8_t** options, size_t* options_size) {
    if (!packet || packet_size < sizeof(struct dhcp_header)) return -1;
    
    const struct dhcp_header* hdr = (const struct dhcp_header*)packet;
    
    // Проверяем magic cookie
    if (ntohl(hdr->magic) != DHCP_MAGIC_COOKIE) return -1;
    
    if (header) {
        *header = *hdr;
    }
    
    if (options && options_size) {
        *options = (const uint8_t*)packet + sizeof(struct dhcp_header);
        *options_size = packet_size - sizeof(struct dhcp_header);
    }
    
    return 0;
}

// Обработать DHCP OFFER
static int dhcp_handle_offer(const struct dhcp_header* header, const uint8_t* options, size_t options_size) {
    if (!header || !options) {
        dhcp_print("\n[DHCP] ERROR: Invalid OFFER packet");
        return -1;
    }
    
    // Проверяем transaction ID
    if (ntohl(header->xid) != dhcp_xid) {
        dhcp_print("\n[DHCP] WARNING: OFFER transaction ID mismatch");
        return -1;
    }
    
    // Проверяем тип сообщения
    uint8_t msg_type_len;
    const uint8_t* msg_type_ptr = dhcp_find_option(options, options_size, DHCP_OPTION_MESSAGE_TYPE, &msg_type_len);
    if (!msg_type_ptr || msg_type_len != 1 || *msg_type_ptr != DHCP_MESSAGE_TYPE_OFFER) {
        dhcp_print("\n[DHCP] ERROR: Invalid message type in OFFER");
        return -1;
    }
    
    // Извлекаем предложенный IP
    uint32_t offered_ip = ntohl(header->yiaddr);
    if (offered_ip == 0) {
        dhcp_print("\n[DHCP] ERROR: OFFER contains invalid IP (0.0.0.0)");
        return -1;
    }
    
    // Извлекаем опции
    uint32_t subnet_mask = 0xFFFFFF00; // По умолчанию /24
    uint32_t gateway = 0;
    uint32_t dns_server = 0;
    uint32_t server_ip = ntohl(header->siaddr);
    
    uint8_t opt_len;
    const uint8_t* opt_data;
    
    // Subnet Mask
    opt_data = dhcp_find_option(options, options_size, DHCP_OPTION_SUBNET_MASK, &opt_len);
    if (opt_data && opt_len == 4) {
        subnet_mask = ntohl(*(const uint32_t*)opt_data);
    }
    
    // Router (Gateway)
    opt_data = dhcp_find_option(options, options_size, DHCP_OPTION_ROUTER, &opt_len);
    if (opt_data && opt_len >= 4) {
        gateway = ntohl(*(const uint32_t*)opt_data);
    }
    
    // DNS Server
    opt_data = dhcp_find_option(options, options_size, DHCP_OPTION_DNS_SERVER, &opt_len);
    if (opt_data && opt_len >= 4) {
        dns_server = ntohl(*(const uint32_t*)opt_data);
    }
    
    // Server Identifier (если не в siaddr)
    opt_data = dhcp_find_option(options, options_size, DHCP_OPTION_SERVER_IDENTIFIER, &opt_len);
    if (opt_data && opt_len == 4) {
        server_ip = ntohl(*(const uint32_t*)opt_data);
    }
    
    // Сохраняем предложенную конфигурацию
    dhcp_offered_ip = offered_ip;
    dhcp_server_ip = server_ip;
    
    dhcp_print("\n[DHCP] OFFER received");
    return 0;
}

// Обработать DHCP ACK
static int dhcp_handle_ack(const struct dhcp_header* header, const uint8_t* options, size_t options_size) {
    if (!header || !options) {
        dhcp_print("\n[DHCP] ERROR: Invalid ACK packet");
        return -1;
    }
    
    // Проверяем transaction ID
    if (ntohl(header->xid) != dhcp_xid) {
        dhcp_print("\n[DHCP] WARNING: ACK transaction ID mismatch");
        return -1;
    }
    
    // Проверяем тип сообщения
    uint8_t msg_type_len;
    const uint8_t* msg_type_ptr = dhcp_find_option(options, options_size, DHCP_OPTION_MESSAGE_TYPE, &msg_type_len);
    if (!msg_type_ptr || msg_type_len != 1 || *msg_type_ptr != DHCP_MESSAGE_TYPE_ACK) {
        dhcp_print("\n[DHCP] ERROR: Invalid message type in ACK");
        return -1;
    }
    
    // Извлекаем IP адрес
    uint32_t ip_address = ntohl(header->yiaddr);
    if (ip_address == 0) {
        dhcp_print("\n[DHCP] ERROR: ACK contains invalid IP (0.0.0.0)");
        return -1;
    }
    
    // Извлекаем опции
    uint32_t subnet_mask = 0xFFFFFF00;
    uint32_t gateway = 0;
    uint32_t dns_server = 0;
    uint32_t lease_time = 86400; // По умолчанию 24 часа
    uint32_t server_ip = ntohl(header->siaddr);
    
    uint8_t opt_len;
    const uint8_t* opt_data;
    
    opt_data = dhcp_find_option(options, options_size, DHCP_OPTION_SUBNET_MASK, &opt_len);
    if (opt_data && opt_len == 4) {
        subnet_mask = ntohl(*(const uint32_t*)opt_data);
    }
    
    opt_data = dhcp_find_option(options, options_size, DHCP_OPTION_ROUTER, &opt_len);
    if (opt_data && opt_len >= 4) {
        gateway = ntohl(*(const uint32_t*)opt_data);
    }
    
    opt_data = dhcp_find_option(options, options_size, DHCP_OPTION_DNS_SERVER, &opt_len);
    if (opt_data && opt_len >= 4) {
        dns_server = ntohl(*(const uint32_t*)opt_data);
    }
    
    opt_data = dhcp_find_option(options, options_size, DHCP_OPTION_LEASE_TIME, &opt_len);
    if (opt_data && opt_len == 4) {
        lease_time = ntohl(*(const uint32_t*)opt_data);
    }
    
    opt_data = dhcp_find_option(options, options_size, DHCP_OPTION_SERVER_IDENTIFIER, &opt_len);
    if (opt_data && opt_len == 4) {
        server_ip = ntohl(*(const uint32_t*)opt_data);
    }

    if (dns_server == 0) {
        dns_server = 0x0A000203;
    }
    
    // Применяем конфигурацию
    network_apply_config(ip_address, subnet_mask, gateway, dns_server);
    
    // Сохраняем конфигурацию
    dhcp_config.ip_address = ip_address;
    dhcp_config.subnet_mask = subnet_mask;
    dhcp_config.gateway = gateway;
    dhcp_config.dns_server = dns_server;
    dhcp_config.server_ip = server_ip;
    dhcp_config.lease_time = lease_time;
    dhcp_config.valid = true;
    bool was_renew = dhcp_renew_in_progress;
    dhcp_bound_at = tcp_get_time();
    dhcp_renew_in_progress = false;

    if (was_renew) {
        log_msg(LOG_DBG, "dhcp", "renew ACK");
    } else {
        log_ip(LOG_INFO, "dhcp", "ACK applied", ip_address);
        log_fmt3(LOG_INFO, "dhcp", "lease", "sec", lease_time, "gw", gateway, "mask", subnet_mask);
        /* Не пишем /etc на каждый ACK: на boot IRQ ещё off, AHCI write
           через WSL медленный; save — через network_config_save / shell. */
        dhcp_print("\n[DHCP] ACK received, configuration applied");
    }
    return 0;
}

// Инициализация DHCP
void dhcp_init() {
    dhcp_current_state = DHCP_STATE_IDLE;
    dhcp_config.valid = false;
    dhcp_xid = 0;
    dhcp_server_ip = 0;
    dhcp_offered_ip = 0;
    int pid = g_dhcpd_pid >= 0 ? g_dhcpd_pid : sched_current_id();
    if (pid < 0) pid = NET_PID_DHCPD;
    net_ports_register(NET_PROTO_UDP, NETPORT_LISTEN,
                       0, DHCP_CLIENT_PORT, 0, 0,
                       -1, pid, "dhcpd");
}

// Обработать входящий DHCP пакет
void dhcp_handle_packet(uint32_t src_ip, uint16_t src_port,
                       const void* udp_payload, size_t payload_size) {
    if (!udp_payload || payload_size < sizeof(struct dhcp_header)) return;

    log_fmt3(dhcp_verbose ? LOG_INFO : LOG_DBG, "dhcp", "udp packet", "from", src_ip, "sport", (uint32_t)src_port, "len", (uint32_t)payload_size);
    
    struct dhcp_header header;
    const uint8_t* options;
    size_t options_size;
    
    if (dhcp_parse_packet(udp_payload, payload_size, &header, &options, &options_size) != 0) {
        return;
    }
    
    // Проверяем, что это ответ (BOOTREPLY)
    if (header.op != 2) return;
    
    // Проверяем тип сообщения
    uint8_t msg_type_len;
    const uint8_t* msg_type_ptr = dhcp_find_option(options, options_size, DHCP_OPTION_MESSAGE_TYPE, &msg_type_len);
    if (!msg_type_ptr || msg_type_len != 1) return;
    
    uint8_t msg_type = *msg_type_ptr;
    
    if (msg_type == DHCP_MESSAGE_TYPE_OFFER && dhcp_current_state == DHCP_STATE_DISCOVERING) {
        log_ip(dhcp_verbose ? LOG_INFO : LOG_DBG, "dhcp", "OFFER received", src_ip);
        if (dhcp_handle_offer(&header, options, options_size) == 0) {
            // Отправляем REQUEST
            dhcp_current_state = DHCP_STATE_REQUESTING;
            dhcp_send_request(dhcp_offered_ip, dhcp_server_ip);
        }
    } else if (msg_type == DHCP_MESSAGE_TYPE_ACK &&
               (dhcp_current_state == DHCP_STATE_REQUESTING || dhcp_current_state == DHCP_STATE_BOUND)) {
        if (dhcp_handle_ack(&header, options, options_size) == 0) {
            dhcp_current_state = DHCP_STATE_BOUND;
        }
    } else if (msg_type == DHCP_MESSAGE_TYPE_NAK) {
        if (dhcp_renew_in_progress) {
            dhcp_renew_in_progress = false;
            return;
        }
        dhcp_print("\n[DHCP] ERROR: NAK received (server rejected request)");
        dhcp_current_state = DHCP_STATE_FAILED;
    }
}

// Получить IP через DHCP (блокирующая функция)
static int dhcp_do_acquire(void) {
    if (dhcp_current_state == DHCP_STATE_BOUND) {
        dhcp_print("\n[DHCP] Already bound");
        return 0;
    }

    dhcp_print("\n[DHCP] Starting IP acquisition...");
    dhcp_init();
    dhcp_current_state = DHCP_STATE_DISCOVERING;
    
    // Отправляем DISCOVER
    if (dhcp_send_discover() != 0) {
        dhcp_print("\n[DHCP] ERROR: Failed to send DISCOVER packet");
        dhcp_current_state = DHCP_STATE_FAILED;
        return -1;
    }
    
    // Ждем ответ (polling)
    int attempts = 0;
    int max_attempts = 100;
    int discover_retries = 0;
    int request_retries = 0;
    const int MAX_DISCOVER_RETRIES = 3;
    const int MAX_REQUEST_RETRIES = 3;
    
    while (dhcp_current_state == DHCP_STATE_DISCOVERING || dhcp_current_state == DHCP_STATE_REQUESTING) {
        if (attempts > 0 && (attempts % 10) == 0) {
            uint8_t cmd = 0;
            uint16_t isr = 0, capr = 0, cbr = 0;
            nic_debug_rx_regs(&cmd, &isr, &capr, &cbr);
            log_fmt3(LOG_INFO, "dhcp", "poll", "cmd", (uint32_t)cmd, "isr", (uint32_t)isr, "capr", (uint32_t)capr);
            log_fmt3(LOG_INFO, "dhcp", "poll ring", "cbr", (uint32_t)cbr, "bufempty", (uint32_t)((cmd & 0x01) ? 0u : 1u), "ok", 1u);
        }

        nic_process_packets();
        if (dhcp_current_state == DHCP_STATE_BOUND) {
            break;
        }
        extern void net_wait_ms(uint32_t ms);
        net_wait_ms(10);
        if (dhcp_current_state == DHCP_STATE_BOUND) {
            break;
        }
        attempts++;
        if (attempts >= max_attempts) {
            // Повторяем DISCOVER
            if (dhcp_current_state == DHCP_STATE_DISCOVERING) {
                discover_retries++;
                if (discover_retries >= MAX_DISCOVER_RETRIES) {
                    dhcp_print("\n[DHCP] ERROR: Timeout waiting for OFFER (no DHCP server response)");
                    dhcp_current_state = DHCP_STATE_FAILED;
                    return -1;
                }
                dhcp_print("\n[DHCP] Retrying DISCOVER...");
                if (dhcp_send_discover() != 0) {
                    dhcp_print("\n[DHCP] ERROR: Failed to resend DISCOVER");
                    dhcp_current_state = DHCP_STATE_FAILED;
                    return -1;
                }
                attempts = 0;
            } else if (dhcp_current_state == DHCP_STATE_REQUESTING) {
                request_retries++;
                if (request_retries >= MAX_REQUEST_RETRIES) {
                    dhcp_print("\n[DHCP] ERROR: Timeout waiting for ACK (server did not confirm IP)");
                    dhcp_current_state = DHCP_STATE_FAILED;
                    return -1;
                }
                dhcp_print("\n[DHCP] Retrying REQUEST...");
                if (dhcp_send_request(dhcp_offered_ip, dhcp_server_ip) != 0) {
                    dhcp_print("\n[DHCP] ERROR: Failed to resend REQUEST");
                    dhcp_current_state = DHCP_STATE_FAILED;
                    return -1;
                }
                attempts = 0;
            }
        }
    }
    
    if (dhcp_current_state == DHCP_STATE_BOUND) {
        dhcp_print("\n[DHCP] Successfully obtained IP address");
        return 0;
    }
    
    if (dhcp_current_state == DHCP_STATE_FAILED) {
        dhcp_print("\n[DHCP] ERROR: Failed to obtain IP address");
    } else {
        dhcp_print("\n[DHCP] ERROR: Unknown state, failed to obtain IP");
    }

    return -1;
}

int dhcp_acquire(void) {
    dhcp_verbose = true;
    int r = dhcp_do_acquire();
    dhcp_verbose = false;
    return r;
}

int dhcp_acquire_quiet(void) {
    dhcp_verbose = false;
    return dhcp_do_acquire();
}

// Получить конфигурацию
int dhcp_get_config(struct dhcp_config* config) {
    if (!config || !dhcp_config.valid) return -1;
    *config = dhcp_config;
    return 0;
}

// Освободить аренду
int dhcp_release() {
    if (dhcp_current_state == DHCP_STATE_BOUND && dhcp_config.valid) {
        dhcp_send_release();
    }
    dhcp_current_state = DHCP_STATE_IDLE;
    dhcp_config.valid = false;
    ip_set_our_ip(0);
    return 0;
}

// Получить состояние
enum dhcp_state dhcp_get_state() {
    return dhcp_current_state;
}

bool dhcp_accepts_dest_ip(uint32_t dest_ip) {
    (void)dest_ip;
    if (dhcp_current_state == DHCP_STATE_DISCOVERING) {
        return true;
    }
    if (dhcp_current_state == DHCP_STATE_REQUESTING &&
        dhcp_offered_ip != 0 && dest_ip == dhcp_offered_ip) {
        return true;
    }
    return false;
}

void dhcp_poll() {
    if (!dhcp_config.valid || dhcp_config.lease_time == 0) {
        return;
    }

    uint32_t now = tcp_get_time();
    // lease_time is seconds; renew at half-lease (milliseconds)
    uint32_t half_lease_ticks = (dhcp_config.lease_time / 2) * 1000;
    if (half_lease_ticks < 30000) {
        half_lease_ticks = 30000;
    }

    if (dhcp_renew_in_progress) {
        if (now - dhcp_renew_sent_at > DHCP_RENEW_TIMEOUT_TICKS) {
            dhcp_renew_in_progress = false;
        }
        return;
    }

    if (dhcp_current_state != DHCP_STATE_BOUND) {
        return;
    }

    if (now - dhcp_bound_at < half_lease_ticks) {
        return;
    }

    dhcp_xid = dhcp_generate_xid();
    dhcp_renew_in_progress = true;
    dhcp_renew_sent_at = now;
    if (dhcp_send_request(dhcp_config.ip_address, dhcp_config.server_ip) != 0) {
        dhcp_renew_in_progress = false;
    }
}

static void dhcpd_task(void* arg) {
    (void)arg;
    g_dhcpd_pid = sched_current_id();
    net_ports_register(NET_PROTO_UDP, NETPORT_LISTEN,
                       0, DHCP_CLIENT_PORT, 0, 0,
                       -1, g_dhcpd_pid, "dhcpd");
    log_fmt3(LOG_INFO, "autotest", "dhcpd_kthread_ok", "pid", (uint32_t)g_dhcpd_pid,
             "ok", 1u, "x", 0u);
    while (1) {
        dhcp_poll();
        task_sleep_ms(50);
    }
}

int dhcp_service_pid(void) {
    return g_dhcpd_pid;
}

int dhcp_start_service(void) {
    if (g_dhcpd_pid >= 0 && task_get_state(g_dhcpd_pid) != TASK_UNUSED &&
        task_get_state(g_dhcpd_pid) != TASK_ZOMBIE) {
        return g_dhcpd_pid;
    }
    g_dhcpd_pid = -1;
    int id = task_create(dhcpd_task, 0, "dhcpd");
    if (id < 0) return -1;
    /* Yield until dhcpd sets pid / logs */
    for (int i = 0; i < 64 && g_dhcpd_pid < 0; i++) sched_yield();
    return id;
}
