#include "dev.h"
#include "fs.h"
#include "drivers/storage/disk_manager.h"
#include "serial_log.h"
#include <stdint.h>
#include <stddef.h>

// Инициализация виртуальной файловой системы /dev
int dev_init() {
    // /dev обычно уже есть после fs_init; create_dir — no-op если есть.
    fs_create_dir("/dev");

    /* Не пишем /dev/<disk> файлы на каждый boot: fs_write → save file table
       (~136 секторов AHCI) вешал boot до sti / на медленном WSL img. */
    int disk_count = disk_manager_count();
    log_fmt3(LOG_DBG, "dev", "init", "disks", (uint32_t)disk_count, "nodes", 0u, "ok", 1u);
    (void)disk_count;
    return 0;
}

