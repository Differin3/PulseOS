#include "drivers/pci/pci.h"
#include "drivers/video/terminal.h"
#include <stdint.h>
#include <stddef.h>

// Порты ввода/вывода
static inline void outl(uint16_t port, uint32_t val) { asm volatile ("outl %0, %1" : : "a"(val), "Nd"(port)); }
static inline uint32_t inl(uint16_t port) { uint32_t ret; asm volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port)); return ret; }

// Чтение из PCI config space
uint32_t pci_read_config(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = (uint32_t)((1 << 31) | (bus << 16) | (device << 11) | (function << 8) | (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

// Запись в PCI config space
void pci_write_config(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)((1 << 31) | (bus << 16) | (device << 11) | (function << 8) | (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

// Проверка существования устройства
static int pci_device_exists(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t vendor_id = pci_read_config(bus, device, function, 0);
    return (vendor_id != 0xFFFFFFFF);
}

// Получить базовый адрес памяти (только для memory BAR)
uint32_t pci_get_bar(uint8_t bus, uint8_t device, uint8_t function, uint8_t bar_num) {
    if (bar_num > 5) return 0;
    uint32_t bar = pci_read_config(bus, device, function, 0x10 + bar_num * 4);
    if (bar & 1) return 0; // I/O порт, не память
    return bar & 0xFFFFFFF0; // Маска для адреса
}

// Получить базовый адрес (memory или I/O)
uint32_t pci_get_bar_any(uint8_t bus, uint8_t device, uint8_t function, uint8_t bar_num) {
    if (bar_num > 5) return 0;
    uint32_t bar = pci_read_config(bus, device, function, 0x10 + bar_num * 4);
    if (bar & 1) {
        // I/O порт - возвращаем адрес порта
        return bar & 0xFFFFFFFC;
    } else {
        // Memory - возвращаем адрес памяти
        return bar & 0xFFFFFFF0;
    }
}

// Вспомогательные функции для отладки (закомментированы, используются только при необходимости)
/*
static void print_hex(uint32_t val) {
    const char hex_chars[] = "0123456789ABCDEF";
    terminal_writestring("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uint8_t nibble = (val >> i) & 0xF;
        char hex_char[2] = {hex_chars[nibble], 0};
        terminal_writestring(hex_char);
    }
}

static void print_num(uint32_t val) {
    char buf[12];
    int p = 0;
    if (val == 0) buf[p++] = '0';
    else {
        char tmp[12]; int t = 0;
        uint32_t n = val;
        while (n > 0 && t < 11) { tmp[t++] = '0' + (n % 10); n /= 10; }
        while (t > 0) buf[p++] = tmp[--t];
    }
    buf[p] = 0;
    terminal_writestring(buf);
}
*/

// Поиск PCI устройства по классу и подклассу
int pci_find_device(uint8_t class_code, uint8_t subclass, struct pci_device* dev) {
    // Сканируем первые 16 шин
    for (int bus = 0; bus < 16; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            for (uint8_t function = 0; function < 8; function++) {
                if (!pci_device_exists(bus, device, function)) continue;
                
                uint32_t class_reg = pci_read_config(bus, device, function, 0x08);
                uint8_t dev_class = (class_reg >> 24) & 0xFF;
                uint8_t dev_subclass = (class_reg >> 16) & 0xFF;
                
                if (dev_class == class_code && dev_subclass == subclass) {
                    uint32_t vendor_device = pci_read_config((uint8_t)bus, device, function, 0x00);
                    dev->vendor_id = vendor_device & 0xFFFF;
                    dev->device_id = (vendor_device >> 16) & 0xFFFF;
                    dev->bus = (uint8_t)bus;
                    dev->device = device;
                    dev->function = function;
                    
                    // Читаем BAR регистры напрямую (сохраняем полный BAR включая бит I/O space)
                    for (int i = 0; i < 6; i++) {
                        dev->base_address[i] = pci_read_config((uint8_t)bus, device, function, 0x10 + i * 4);
                    }
                    
                    return 0;
                }
            }
        }
    }
    
    return -1; // устройство не найдено
}

