#include "mount.h"
#include "drivers/storage/disk_manager.h"
#include "fs.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

static struct mount_point mount_table[MAX_MOUNTS];
static int mount_count = 0;

// Инициализация системы монтирования
int mount_init() {
    mount_count = 0;
    
    // Монтируем первый диск в корень
    if (disk_manager_count() > 0) {
        disk_select(0);
        mount_table[0].disk_id = 0;
        mount_table[0].partition_id = 0;
        mount_table[0].fs_type = 0; // Наша ФС
        mount_table[0].active = true;
        mount_table[0].path[0] = '/';
        mount_table[0].path[1] = 0;
        mount_count = 1;
    }
    
    return mount_count;
}

// Монтировать файловую систему
int mount_fs(int disk_id, const char* path) {
    if (mount_count >= MAX_MOUNTS) return -1;
    if (disk_id < 0 || disk_id >= disk_manager_count()) return -1;
    
    if (disk_select(disk_id) != 0) return -1;
    
    // Проверяем, не смонтирован ли уже этот диск
    for (int i = 0; i < mount_count; i++) {
        if (mount_table[i].disk_id == disk_id && mount_table[i].active) {
            return -1; // Уже смонтирован
        }
    }
    
    // Добавляем точку монтирования
    int i = 0;
    while (path[i] && i < 63) {
        mount_table[mount_count].path[i] = path[i];
        i++;
    }
    mount_table[mount_count].path[i] = 0;
    mount_table[mount_count].disk_id = disk_id;
    mount_table[mount_count].partition_id = 0;
    mount_table[mount_count].fs_type = 0;
    mount_table[mount_count].active = true;
    mount_count++;
    
    return 0;
}

// Размонтировать файловую систему
int unmount_fs(const char* path) {
    for (int i = 0; i < mount_count; i++) {
        int j = 0;
        while (path[j] && mount_table[i].path[j] && path[j] == mount_table[i].path[j]) j++;
        if (!path[j] && !mount_table[i].path[j]) {
            // Найдена точка монтирования
            if (mount_table[i].path[0] == '/' && mount_table[i].path[1] == 0) {
                return -1; // Нельзя размонтировать корень
            }
            mount_table[i].active = false;
            return 0;
        }
    }
    return -1; // Не найдено
}

// Найти точку монтирования для пути
const struct mount_point* get_mount_for_path(const char* path) {
    const struct mount_point* best_match = 0;
    int best_match_len = 0;
    
    for (int i = 0; i < mount_count; i++) {
        if (!mount_table[i].active) continue;
        
        int j = 0;
        while (path[j] && mount_table[i].path[j] && path[j] == mount_table[i].path[j]) j++;
        
        // Если путь начинается с точки монтирования
        if (!mount_table[i].path[j] && j > best_match_len) {
            best_match = &mount_table[i];
            best_match_len = j;
        }
    }
    
    return best_match ? best_match : &mount_table[0]; // По умолчанию корень
}

