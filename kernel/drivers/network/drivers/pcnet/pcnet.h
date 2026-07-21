#ifndef PCNET_H
#define PCNET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct pci_device;
struct nic_device; // from nic.h

// Инициализация AMD PCnet (0x1022:0x2000) через PCI
int pcnet_init_with_pci(const struct pci_device* dev, struct nic_device* nic);

// Передача пакета через PCnet
int pcnet_send_packet(struct nic_device* nic, const void* data, size_t len);

// Получение пакета (0 если нет данных)
int pcnet_receive_packet(struct nic_device* nic, void* buffer, size_t max_len);

// Проверка наличия пакета
bool pcnet_has_packet(struct nic_device* nic);

#endif // PCNET_H
