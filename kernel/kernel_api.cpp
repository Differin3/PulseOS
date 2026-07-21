#include "kernel_api.h"
#include "driver_manager.h"

// Чтение из устройства
int kernel_device_read(uint32_t device_id, void* buffer, size_t size, uint32_t offset) {
    return driver_read(device_id, buffer, size, offset);
}

// Запись в устройство
int kernel_device_write(uint32_t device_id, const void* buffer, size_t size, uint32_t offset) {
    return driver_write(device_id, buffer, size, offset);
}

// Управление устройством
int kernel_device_ioctl(uint32_t device_id, uint32_t cmd, void* arg) {
    return driver_ioctl(device_id, cmd, arg);
}

// Получить список устройств по типу
int kernel_device_list(enum driver_type type, struct driver* devices, int max_count) {
    return driver_list_by_type(type, devices, max_count);
}

// Получить информацию об устройстве
struct driver* kernel_device_get_info(uint32_t device_id) {
    return driver_find_by_id(device_id);
}

// Найти устройство по имени
struct driver* kernel_device_find_by_name(const char* name) {
    return driver_find_by_name(name);
}

// Найти устройство по типу и индексу
struct driver* kernel_device_find_by_type(enum driver_type type, int index) {
    return driver_find_by_type(type, index);
}
