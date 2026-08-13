#ifndef NIC_H
#define NIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ETH_MAX_PACKET_SIZE 1514

enum nic_hw_type { NIC_HW_NONE = 0, NIC_HW_RTL8139, NIC_HW_PCNET, NIC_HW_VIRTIO };

struct nic_device {
    uint16_t io_base;
    volatile uint8_t* mem_base;
    uint8_t mac_address[6];
    uint32_t device_id;
    bool initialized;
    bool active;
    uint8_t* rx_buffer;
    uint8_t* tx_buffer;
    uint16_t rx_current;
    enum nic_hw_type hw_type;
    uint8_t irq_line;
    uint8_t pci_bus;
    uint8_t pci_device;
    uint8_t pci_function;
};

#define NIC_IOCTL_GET_MAC  0
#define NIC_IOCTL_SET_IP   1
#define NIC_IOCTL_GET_IP   2
#define NIC_IOCTL_UDP_SEND 3

struct pci_device;

int nic_init();
int nic_init_with_device(const struct pci_device* dev);

int nic_send_packet(const void* data, size_t len);
int nic_receive_packet(void* buffer, size_t max_len);
void nic_get_mac(uint8_t* mac);
bool nic_has_packet();
void nic_process_packets();
void nic_debug_rx_regs(uint8_t* cmd, uint16_t* isr, uint16_t* capr, uint16_t* cbr);

int find_rtl8139(struct pci_device* dev);

struct nic_device* nic_get_device(void);

#endif
