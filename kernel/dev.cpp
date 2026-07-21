#include "dev.h"
#include "fs.h"
#include "drivers/storage/disk_manager.h"
#include <stdint.h>
#include <stddef.h>

// Инициализация виртуальной файловой системы /dev
int dev_init() {
    // Создаем директорию /dev если её нет
    fs_create_dir("/dev");
    
    // Создаем узлы устройств для всех дисков
    int disk_count = disk_manager_count();
    for (int i = 0; i < disk_count; i++) {
        const struct disk_info* disk = disk_manager_get_disk(i);
        if (!disk) continue;
        
        // Создаем узел устройства /dev/sda, /dev/sdb и т.д.
        char dev_path[32];
        dev_path[0] = '/';
        dev_path[1] = 'd';
        dev_path[2] = 'e';
        dev_path[3] = 'v';
        dev_path[4] = '/';
        int j = 5;
        int k = 0;
        while (disk->name[k] && j < 31) {
            dev_path[j++] = disk->name[k++];
        }
        dev_path[j] = 0;
        
        // Создаем специальный файл (пока просто записываем информацию о диске)
        char disk_info[128];
        int pos = 0;
        
        // Формируем строку с информацией о диске
        const char* info_prefix = "Disk device: ";
        k = 0;
        while (info_prefix[k] && pos < 127) disk_info[pos++] = info_prefix[k++];
        k = 0;
        while (disk->name[k] && pos < 127) disk_info[pos++] = disk->name[k++];
        disk_info[pos] = 0;
        
        // Записываем как файл (в будущем можно сделать специальные узлы)
        fs_write(dev_path, disk_info, pos);
    }
    
    return 0;
}

