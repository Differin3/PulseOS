#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct pci_device;
struct nic_device; // from nic.h

// Инициализация RTL8139 по PCI
int rtl8139_init_with_pci(const struct pci_device* dev, struct nic_device* nic);

// Передача/прием пакетов
int rtl8139_send_packet(struct nic_device* nic, const void* data, size_t len);
int rtl8139_receive_packet(struct nic_device* nic, void* buffer, size_t max_len);
bool rtl8139_has_packet(struct nic_device* nic);

// Снимок регистров RX для отладки (CMD, ISR, CAPR, CBR)
void rtl8139_rx_regs(struct nic_device* nic, uint8_t* cmd, uint16_t* isr, uint16_t* capr, uint16_t* cbr);

// Поиск RTL8139 через PCI (для driver_manager)
int find_rtl8139(struct pci_device* dev);

void rtl8139_enable_irq(struct nic_device* nic);
void rtl8139_handle_irq(struct nic_device* nic);

#endif // RTL8139_H
