#include "driver_manager.h"
#include "drivers/pci/pci.h"
#include "serial_log.h"
#include "drivers/storage/ahci.h"
#include "drivers/storage/nvme.h"
#include "drivers/storage/ata.h"
#include "drivers/input/keyboard.h"
#include "drivers/video/terminal.h"
#include "drivers/network/nic.h"
#include <stddef.h>

static struct driver driver_list[MAX_DRIVERS];
static int driver_count_internal = 0;
static bool driver_manager_initialized = false;
static uint32_t next_device_id = 1;  // Счетчик для уникальных ID устройств

// Инициализация менеджера драйверов
int driver_manager_init() {
    if (driver_manager_initialized) return 0;
    
    // Очищаем список драйверов
    for (int i = 0; i < MAX_DRIVERS; i++) {
        driver_list[i].name[0] = 0;
        driver_list[i].initialized = false;
        driver_list[i].active = false;
        driver_list[i].device_id = 0;
        driver_list[i].device_data = 0;
    }
    
    driver_count_internal = 0;
    next_device_id = 1;
    driver_manager_initialized = true;
    log_msg(LOG_INFO, "driver", "manager initialized");
    
    return 0;
}

// Регистрация драйвера
int driver_register(struct driver* drv) {
    if (!driver_manager_initialized) return -1;
    if (driver_count_internal >= MAX_DRIVERS) return -1;
    if (!drv || !drv->name[0]) return -1;
    
    // Проверяем, нет ли уже драйвера с таким именем
    for (int i = 0; i < driver_count_internal; i++) {
        int j = 0;
        while (driver_list[i].name[j] && drv->name[j] && 
               driver_list[i].name[j] == drv->name[j]) j++;
        if (!driver_list[i].name[j] && !drv->name[j]) {
            return -1; // Драйвер с таким именем уже зарегистрирован
        }
    }
    
    // Если device_id не установлен, присваиваем автоматически
    if (drv->device_id == 0) {
        drv->device_id = next_device_id++;
    }
    
    // Копируем драйвер в список
    int idx = driver_count_internal++;
    int i = 0;
    while (drv->name[i] && i < 31) {
        driver_list[idx].name[i] = drv->name[i];
        i++;
    }
    driver_list[idx].name[i] = 0;
    driver_list[idx].type = drv->type;
    driver_list[idx].device_id = drv->device_id;
    driver_list[idx].device_data = drv->device_data;
    driver_list[idx].ops = drv->ops;
    driver_list[idx].initialized = drv->initialized;
    driver_list[idx].active = drv->active;

    if (drv->initialized) {
        log_driver_event(driver_list[idx].name, "registered");
    } else {
        log_driver_event(driver_list[idx].name, "registered (not init)");
    }
    
    return 0;
}

// Поиск драйвера по типу и индексу
struct driver* driver_find_by_type(enum driver_type type, int index) {
    if (!driver_manager_initialized) return 0;
    
    int found = 0;
    for (int i = 0; i < driver_count_internal; i++) {
        if (driver_list[i].type == type && driver_list[i].initialized) {
            if (found == index) {
                return &driver_list[i];
            }
            found++;
        }
    }
    
    return 0;
}

// Поиск драйвера по имени
struct driver* driver_find_by_name(const char* name) {
    if (!driver_manager_initialized || !name) return 0;
    
    for (int i = 0; i < driver_count_internal; i++) {
        int j = 0;
        while (driver_list[i].name[j] && name[j] && 
               driver_list[i].name[j] == name[j]) j++;
        if (!driver_list[i].name[j] && !name[j]) {
            return &driver_list[i];
        }
    }
    
    return 0;
}

// Поиск драйвера по ID устройства
struct driver* driver_find_by_id(uint32_t device_id) {
    if (!driver_manager_initialized) return 0;
    
    for (int i = 0; i < driver_count_internal; i++) {
        if (driver_list[i].device_id == device_id) {
            return &driver_list[i];
        }
    }
    
    return 0;
}

// Получить список драйверов по типу
int driver_list_by_type(enum driver_type type, struct driver* drivers, int max_count) {
    if (!driver_manager_initialized || !drivers) return 0;
    
    int count = 0;
    for (int i = 0; i < driver_count_internal && count < max_count; i++) {
        if (driver_list[i].type == type && driver_list[i].initialized) {
            drivers[count] = driver_list[i];
            count++;
        }
    }
    
    return count;
}

// Получить общее количество зарегистрированных драйверов
int driver_count() {
    return driver_count_internal;
}

// Автоматическое сканирование и регистрация устройств
int driver_scan_devices() {
    if (!driver_manager_initialized) return -1;

    log_msg(LOG_INFO, "scan", "PCI / storage / network");
    
    // Сканируем storage устройства через PCI
    // NVMe контроллеры (class 0x01, subclass 0x08)
    struct pci_device nvme_dev;
    int nvme_idx = 0;
    while (pci_find_device(0x01, 0x08, &nvme_dev) == 0 && nvme_idx < 4) {
        extern int nvme_init();
        nvme_init();
        nvme_idx++;
    }
    
    // AHCI контроллеры (class 0x01, subclass 0x06)
    struct pci_device ahci_dev;
    if (pci_find_device(0x01, 0x06, &ahci_dev) == 0) {
        ahci_init_with_device(&ahci_dev);
    }
    
    // ATA устройства сканируются напрямую через порты, не через PCI (ata_init() вызывается позже)
    
    // Инициализация сетевой карты (NIC) через общий слой nic_init()
    extern int nic_init();
    log_msg(LOG_INFO, "scan", "nic_init");
    if (nic_init() == 0) {
        log_msg(LOG_INFO, "scan", "nic_init ok");
    } else {
        log_msg(LOG_ERR, "scan", "nic_init failed");
    }
    
    return 0;
}

// Универсальные функции доступа к устройствам через менеджер
int driver_read(uint32_t device_id, void* buffer, size_t size, uint32_t offset) {
    struct driver* drv = driver_find_by_id(device_id);
    if (!drv || !drv->initialized || !drv->active) return -1;
    if (!drv->ops.read) return -1;
    
    return drv->ops.read(drv->device_data, buffer, size, offset);
}

int driver_write(uint32_t device_id, const void* buffer, size_t size, uint32_t offset) {
    struct driver* drv = driver_find_by_id(device_id);
    if (!drv || !drv->initialized || !drv->active) return -1;
    if (!drv->ops.write) return -1;
    
    return drv->ops.write(drv->device_data, buffer, size, offset);
}

int driver_ioctl(uint32_t device_id, uint32_t cmd, void* arg) {
    struct driver* drv = driver_find_by_id(device_id);
    if (!drv || !drv->initialized || !drv->active) return -1;
    if (!drv->ops.ioctl) return -1;
    
    return drv->ops.ioctl(drv->device_data, cmd, arg);
}
