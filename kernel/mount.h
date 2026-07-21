#ifndef MOUNT_H
#define MOUNT_H

#include <stdint.h>
#include <stddef.h>

#define MAX_MOUNTS 8

// Точка монтирования
struct mount_point {
    char path[64];
    int disk_id;
    int partition_id;
    uint8_t fs_type; // 0 = наша ФС, другие для будущего
    bool active;
} __attribute__((packed));

// Монтировать файловую систему
int mount_fs(int disk_id, const char* path);

// Размонтировать файловую систему
int unmount_fs(const char* path);

// Найти точку монтирования для пути
const struct mount_point* get_mount_for_path(const char* path);

// Инициализация системы монтирования
int mount_init();

#endif

