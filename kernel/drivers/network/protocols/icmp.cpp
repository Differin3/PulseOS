#include "icmp.h"
#include "ip.h"
#include "ethernet.h"
#include "arp.h"
#include "../nic.h"
#include "tcp_connection.h"
#include <stddef.h>

static inline uint16_t htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort >> 8) & 0xFF);
}

static inline uint16_t ntohs(uint16_t netshort) {
    return ((netshort & 0xFF) << 8) | ((netshort >> 8) & 0xFF);
}

static struct {
    bool waiting;
    uint16_t id;
    uint16_t sequence;
    uint32_t from_ip;
    bool received;
} icmp_pending = { false, 0, 0, 0, false };

static uint16_t icmp_id_counter = 0x4D59; // "MY"

static uint16_t icmp_checksum(void* data, size_t len) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t sum = 0;

    for (size_t i = 0; i + 1 < len; i += 2) {
        sum += ((uint16_t)bytes[i] << 8) | bytes[i + 1];
    }
    if (len & 1u) {
        sum += (uint16_t)bytes[len - 1] << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

static int icmp_send_echo(uint32_t dest_ip, uint16_t id, uint16_t sequence) {
    uint8_t icmp_buffer[sizeof(struct icmp_header) + 32];
    struct icmp_header* hdr = (struct icmp_header*)icmp_buffer;
    hdr->type = ICMP_TYPE_ECHO_REQUEST;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->id = htons(id);
    hdr->sequence = htons(sequence);

    const char* payload = "MYOS PING";
    size_t payload_len = 9;
    for (size_t i = 0; i < payload_len; i++) {
        icmp_buffer[sizeof(struct icmp_header) + i] = payload[i];
    }

    size_t icmp_len = sizeof(struct icmp_header) + payload_len;
    uint16_t csum = icmp_checksum(icmp_buffer, icmp_len);
    hdr->checksum = htons(csum);

    uint32_t src_ip = ip_get_our_ip();
    if (src_ip == 0) {
        return -1;
    }

    uint8_t ip_buffer[sizeof(struct ip_header) + sizeof(icmp_buffer)];
    int ip_len = ip_create_packet(ip_buffer, sizeof(ip_buffer),
                                 src_ip, dest_ip, IP_PROTOCOL_ICMP,
                                 icmp_buffer, icmp_len);
    if (ip_len < 0) {
        return -1;
    }

    uint8_t dest_mac[6];
    uint32_t our_ip = ip_get_our_ip();
    if (dest_ip == our_ip) {
        nic_get_mac(dest_mac);
    } else {
        uint32_t arp_ip = ip_resolve_next_hop(dest_ip);
        if (arp_ip == 0xFFFFFFFF) {
            for (int i = 0; i < 6; i++) {
                dest_mac[i] = 0xFF;
            }
        } else if (arp_resolve(arp_ip, dest_mac, 3000) != 0) {
            return -1;
        }
    }

    uint8_t our_mac[6];
    nic_get_mac(our_mac);
    uint8_t frame[sizeof(struct ethernet_header) + sizeof(ip_buffer)];
    int frame_len = ethernet_create_frame(frame, sizeof(frame),
                                         dest_mac, our_mac,
                                         ETH_TYPE_IPV4, ip_buffer, ip_len);
    if (frame_len < 0) {
        return -1;
    }

    return nic_send_packet(frame, frame_len);
}

void icmp_handle_packet(uint32_t src_ip, const void* payload, size_t payload_size) {
    if (!payload || payload_size < sizeof(struct icmp_header)) {
        return;
    }

    const struct icmp_header* hdr = (const struct icmp_header*)payload;
    uint8_t type = hdr->type;
    uint16_t id = ntohs(hdr->id);
    uint16_t seq = ntohs(hdr->sequence);

    if (type == ICMP_TYPE_ECHO_REPLY) {
        if (icmp_pending.waiting && id == icmp_pending.id && seq == icmp_pending.sequence) {
            icmp_pending.from_ip = src_ip;
            icmp_pending.received = true;
        }
        return;
    }

    if (type == ICMP_TYPE_DEST_UNREACH || type == ICMP_TYPE_TIME_EXCEEDED) {
        (void)src_ip;
        return;
    }

    if (type == ICMP_TYPE_ECHO_REQUEST) {
        uint8_t reply[sizeof(struct icmp_header) + 64];
        size_t copy_len = payload_size;
        if (copy_len > sizeof(reply)) {
            copy_len = sizeof(reply);
        }
        for (size_t i = 0; i < copy_len; i++) {
            reply[i] = ((const uint8_t*)payload)[i];
        }
        struct icmp_header* rhdr = (struct icmp_header*)reply;
        rhdr->type = ICMP_TYPE_ECHO_REPLY;
        rhdr->code = 0;
        rhdr->checksum = 0;
        uint16_t csum = icmp_checksum(reply, copy_len);
        rhdr->checksum = htons(csum);

        uint32_t our_ip = ip_get_our_ip();
        if (our_ip == 0) {
            return;
        }

        uint8_t ip_buffer[sizeof(struct ip_header) + sizeof(reply)];
        int ip_len = ip_create_packet(ip_buffer, sizeof(ip_buffer),
                                     our_ip, src_ip, IP_PROTOCOL_ICMP,
                                     reply, copy_len);
        if (ip_len < 0) {
            return;
        }

        uint8_t dest_mac[6];
        if (arp_resolve(src_ip, dest_mac, 1000) != 0) {
            return;
        }

        uint8_t our_mac[6];
        nic_get_mac(our_mac);
        uint8_t frame[sizeof(struct ethernet_header) + sizeof(ip_buffer)];
        int frame_len = ethernet_create_frame(frame, sizeof(frame),
                                             dest_mac, our_mac,
                                             ETH_TYPE_IPV4, ip_buffer, ip_len);
        if (frame_len >= 0) {
            nic_send_packet(frame, frame_len);
        }
    }
}

int icmp_ping(uint32_t dest_ip, int count) {
    if (ip_get_our_ip() == 0) {
        return -1;
    }
    if (count < 1) {
        count = 4;
    }
    if (count > 10) {
        count = 10;
    }

    uint16_t ping_id = icmp_id_counter++;
    int received_total = 0;

    for (int n = 0; n < count; n++) {
        icmp_pending.waiting = true;
        icmp_pending.id = ping_id;
        icmp_pending.sequence = (uint16_t)n;
        icmp_pending.from_ip = 0;
        icmp_pending.received = false;

        if (icmp_send_echo(dest_ip, ping_id, (uint16_t)n) != 0) {
            icmp_pending.waiting = false;
            return -1;
        }

        uint32_t start_time = tcp_get_time();
        int attempts = 0;
        const int max_attempts = 50;

        while (attempts < max_attempts) {
            nic_process_packets();
            if (icmp_pending.received) {
                received_total++;
                uint32_t elapsed = tcp_get_time() - start_time;
                extern void terminal_writestring(const char*);
                terminal_writestring("\nReply from ");
                char ip_buf[20];
                ip_format_address(icmp_pending.from_ip, ip_buf, sizeof(ip_buf));
                terminal_writestring(ip_buf);
                terminal_writestring(": seq=");
                char num[8];
                int np = 0;
                int val = n;
                if (val == 0) num[np++] = '0';
                else {
                    char tmp[8];
                    int t = 0;
                    while (val > 0) { tmp[t++] = '0' + (val % 10); val /= 10; }
                    while (t > 0) num[np++] = tmp[--t];
                }
                num[np] = 0;
                terminal_writestring(num);
                terminal_writestring(" time=");
                np = 0;
                val = (int)elapsed;
                if (val == 0) num[np++] = '0';
                else {
                    char tmp[8];
                    int t = 0;
                    while (val > 0) { tmp[t++] = '0' + (val % 10); val /= 10; }
                    while (t > 0) num[np++] = tmp[--t];
                }
                num[np] = 0;
                terminal_writestring(num);
                terminal_writestring("ms");
                break;
            }
            for (volatile int i = 0; i < 100000; i++);
            attempts++;
        }

        if (!icmp_pending.received) {
            extern void terminal_writestring(const char*);
            terminal_writestring("\nRequest timed out");
        }

        icmp_pending.waiting = false;
        for (volatile int i = 0; i < 500000; i++);
    }

    return received_total;
}
