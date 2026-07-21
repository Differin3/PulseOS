#ifndef DRIVER_MANAGER_H
#define DRIVER_MANAGER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Типы драйверов
enum driver_type {
    DRIVER_STORAGE = 0,  // Устройства хранения (AHCI, NVMe, ATA)
    DRIVER_INPUT = 1,    // Устройства ввода (клавиатура, мышь)
    DRIVER_VIDEO = 2,    // Видео устройства (терминал, VGA)
    DRIVER_NETWORK = 3,  // Сетевые устройства
    DRIVER_OTHER = 4     // Другие устройства
};

// Операции драйвера (не все обязательны для всех типов)
struct driver_ops {
    int (*init)(void* device_data);      // Инициализация устройства
    int (*read)(void* device_data, void* buffer, size_t size, uint32_t offset);  // Чтение
    int (*write)(void* device_data, const void* buffer, size_t size, uint32_t offset);  // Запись
    int (*ioctl)(void* device_data, uint32_t cmd, void* arg);  // Управление устройством
    void (*cleanup)(void* device_data);  // Очистка ресурсов
};

// Структура драйвера
struct driver {
    char name[32];              // Имя драйвера (например "ahci", "nvme0")
    enum driver_type type;       // Тип драйвера
    uint32_t device_id;          // Уникальный ID устройства
    void* device_data;           // Данные устройства (специфичные для драйвера)
    struct driver_ops ops;       // Операции драйвера
    bool initialized;            // Флаг инициализации
    bool active;                 // Флаг активности
};

#define MAX_DRIVERS 32  // Максимальное количество драйверов

// Инициализация менеджера драйверов
int driver_manager_init();

// Регистрация драйвера
int driver_register(struct driver* drv);

// Поиск драйвера по типу и индексу
struct driver* driver_find_by_type(enum driver_type type, int index);

// Поиск драйвера по имени
struct driver* driver_find_by_name(const char* name);

// Поиск драйвера по ID устройства
struct driver* driver_find_by_id(uint32_t device_id);

// Получить список драйверов по типу
int driver_list_by_type(enum driver_type type, struct driver* drivers, int max_count);

// Получить общее количество зарегистрированных драйверов
int driver_count();

// Автоматическое сканирование и регистрация устройств
int driver_scan_devices();

// Универсальные функции доступа к устройствам через менеджер
int driver_read(uint32_t device_id, void* buffer, size_t size, uint32_t offset);
int driver_write(uint32_t device_id, const void* buffer, size_t size, uint32_t offset);
int driver_ioctl(uint32_t device_id, uint32_t cmd, void* arg);

#endif
