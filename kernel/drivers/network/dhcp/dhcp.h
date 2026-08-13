#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// DHCP порты
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

// DHCP magic cookie
#define DHCP_MAGIC_COOKIE 0x63825363

// DHCP опции
#define DHCP_OPTION_PAD 0
#define DHCP_OPTION_SUBNET_MASK 1
#define DHCP_OPTION_ROUTER 3
#define DHCP_OPTION_DNS_SERVER 6
#define DHCP_OPTION_HOSTNAME 12
#define DHCP_OPTION_REQUESTED_IP 50
#define DHCP_OPTION_LEASE_TIME 51
#define DHCP_OPTION_MESSAGE_TYPE 53
#define DHCP_OPTION_SERVER_IDENTIFIER 54
#define DHCP_OPTION_PARAMETER_REQUEST_LIST 55
#define DHCP_OPTION_END 255

// DHCP типы сообщений
#define DHCP_MESSAGE_TYPE_DISCOVER 1
#define DHCP_MESSAGE_TYPE_OFFER 2
#define DHCP_MESSAGE_TYPE_REQUEST 3
#define DHCP_MESSAGE_TYPE_DECLINE 4
#define DHCP_MESSAGE_TYPE_ACK 5
#define DHCP_MESSAGE_TYPE_NAK 6
#define DHCP_MESSAGE_TYPE_RELEASE 7
#define DHCP_MESSAGE_TYPE_INFORM 8

// DHCP BOOTP заголовок
struct dhcp_header {
    uint8_t op;          // 1=BOOTREQUEST, 2=BOOTREPLY
    uint8_t htype;       // 1=Ethernet
    uint8_t hlen;        // 6 для Ethernet
    uint8_t hops;
    uint32_t xid;        // Transaction ID
    uint16_t secs;       // Seconds elapsed
    uint16_t flags;      // Broadcast flag (bit 15)
    uint32_t ciaddr;     // Client IP address
    uint32_t yiaddr;     // Your IP address (предложенный сервером)
    uint32_t siaddr;     // Server IP address
    uint32_t giaddr;     // Gateway IP address (relay agent)
    uint8_t chaddr[16];  // Client hardware address (MAC)
    uint8_t sname[64];   // Server name
    uint8_t file[128];   // Boot file name
    uint32_t magic;      // Magic cookie (0x63825363)
} __attribute__((packed));

// DHCP опция
struct dhcp_option {
    uint8_t code;
    uint8_t length;
    uint8_t data[];
} __attribute__((packed));

// DHCP состояние клиента
enum dhcp_state {
    DHCP_STATE_IDLE,
    DHCP_STATE_DISCOVERING,
    DHCP_STATE_REQUESTING,
    DHCP_STATE_BOUND,
    DHCP_STATE_FAILED
};

// DHCP конфигурация (полученная от сервера)
struct dhcp_config {
    uint32_t ip_address;
    uint32_t subnet_mask;
    uint32_t gateway;
    uint32_t dns_server;
    uint32_t server_ip;
    uint32_t lease_time;
    bool valid;
};

// Инициализация DHCP клиента
void dhcp_init();

// Получить IP адрес через DHCP (блокирующая функция, с выводом на экран)
int dhcp_acquire();

// То же без вывода на экран (автозагрузка / renew)
int dhcp_acquire_quiet();

// Обработать входящий DHCP пакет
void dhcp_handle_packet(uint32_t src_ip, uint16_t src_port,
                       const void* udp_payload, size_t payload_size);

// Получить текущую DHCP конфигурацию
int dhcp_get_config(struct dhcp_config* config);

// Освободить DHCP аренду (DHCP RELEASE)
int dhcp_release();

// Получить текущее состояние DHCP
enum dhcp_state dhcp_get_state();

// Принять входящий пакет с dest IP во время DHCP-обмена
bool dhcp_accepts_dest_ip(uint32_t dest_ip);

// Фоновый poll: renew аренды
void dhcp_poll();

/* dhcpd kthread: background renew; returns pid or <0 */
int dhcp_start_service(void);
int dhcp_service_pid(void);

#endif
