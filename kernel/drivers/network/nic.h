#ifndef NIC_H
#define NIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Максимальный размер Ethernet кадра
#define ETH_MAX_PACKET_SIZE 1514

// Тип аппаратного NIC
enum nic_hw_type { NIC_HW_NONE = 0, NIC_HW_RTL8139, NIC_HW_PCNET };

// Универсальное описание сетевого устройства
struct nic_device {
    uint16_t io_base;              // базовый I/O порт (RTL8139/PCnet)
    volatile uint8_t* mem_base;    // MMIO (пока не используется)
    uint8_t mac_address[6];        // MAC
    uint32_t device_id;            // ID в driver_manager
    bool initialized;              // флаг инициализации
    bool active;                   // флаг активности
    uint8_t* rx_buffer;            // RX буфер (используется RTL8139)
    uint8_t* tx_buffer;            // TX буфер (используется RTL8139)
    uint16_t rx_current;           // позиция чтения (RTL8139)
    enum nic_hw_type hw_type;      // тип аппаратного NIC
};

#define NIC_IOCTL_GET_MAC  0
#define NIC_IOCTL_SET_IP   1
#define NIC_IOCTL_GET_IP   2
#define NIC_IOCTL_UDP_SEND 3

// Инициализация NIC (автоопределение по PCI)
int nic_init();
int nic_init_with_device(const struct pci_device* dev);

// Работа с кадрами (0 = успех, -1 = ошибка)
int nic_send_packet(const void* data, size_t len);
int nic_receive_packet(void* buffer, size_t max_len);
void nic_get_mac(uint8_t* mac);
bool nic_has_packet();
void nic_process_packets();
void nic_debug_rx_regs(uint8_t* cmd, uint16_t* isr, uint16_t* capr, uint16_t* cbr);

// Поиск RTL8139 (используется driver_manager)
struct pci_device; // forward
int find_rtl8139(struct pci_device* dev);

#endif
