#ifndef KERNEL_API_H
#define KERNEL_API_H

#include <stdint.h>
#include <stddef.h>
#include "driver_manager.h"

// API ядра для внутреннего использования (не системные вызовы)

// Чтение из устройства
int kernel_device_read(uint32_t device_id, void* buffer, size_t size, uint32_t offset);

// Запись в устройство
int kernel_device_write(uint32_t device_id, const void* buffer, size_t size, uint32_t offset);

// Управление устройством
int kernel_device_ioctl(uint32_t device_id, uint32_t cmd, void* arg);

// Получить список устройств по типу
int kernel_device_list(enum driver_type type, struct driver* devices, int max_count);

// Получить информацию об устройстве
struct driver* kernel_device_get_info(uint32_t device_id);

// Найти устройство по имени
struct driver* kernel_device_find_by_name(const char* name);

// Найти устройство по типу и индексу
struct driver* kernel_device_find_by_type(enum driver_type type, int index);

#endif
