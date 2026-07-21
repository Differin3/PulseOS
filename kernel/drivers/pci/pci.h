#ifndef PCI_H
#define PCI_H

#include <stdint.h>

// PCI конфигурационные порты
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

// Структура PCI устройства
struct pci_device {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint32_t base_address[6];
};

// Чтение из PCI config space
uint32_t pci_read_config(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

// Запись в PCI config space
void pci_write_config(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

// Поиск PCI устройства по классу и подклассу
int pci_find_device(uint8_t class_code, uint8_t subclass, struct pci_device* dev);

// Получить базовый адрес памяти
uint32_t pci_get_bar(uint8_t bus, uint8_t device, uint8_t function, uint8_t bar_num);

#endif

