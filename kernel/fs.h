#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>

// Простая файловая система MOS с битовой картой
#define FS_MAGIC 0x4D4F5320  // "MOS "
#define FS_MAX_FILES 256     // увеличено с 64 до 256
#define FS_FILENAME_LEN 32   // пока оставим 32, можно увеличить позже
#define FS_SECTOR_SIZE 512

// Флаги для fs_file_entry
#define FS_FLAG_FREE      0x00
#define FS_FLAG_OCCUPIED  0x01
#define FS_FLAG_DIRECTORY 0x02

// Структура записи файла (размер ~ 32+4+4+1+4 = 45 байт, округляется до 48)
struct fs_file_entry {
    char name[FS_FILENAME_LEN];
    uint32_t start_sector;      // первый сектор файла (непрерывный блок)
    uint32_t sector_count;      // количество секторов, занятых файлом
    uint32_t size_bytes;        // реальный размер в байтах (может быть меньше sector_count*512)
    uint8_t flags;              // биты: 0=свободен, 1=занят, 2=директория
    uint32_t parent_dir;        // индекс родительской директории (0xFFFFFFFF для корня)
    // можно добавить время, права и т.д.
} __attribute__((packed));

// Загрузочный сектор (суперблок)
struct fs_boot_sector {
    uint32_t magic;
    uint32_t version;
    uint32_t total_sectors;
    uint32_t file_table_sector;
    uint32_t data_start_sector; // начало данных (после таблицы файлов)
    uint32_t file_count;
    uint32_t free_bitmap_sector; // сектор начала битовой карты (в области данных)
    uint32_t free_bitmap_size;   // размер карты в байтах (рассчитывается)
    uint8_t reserved[FS_SECTOR_SIZE - 32];
} __attribute__((packed));

// Инициализация (disk_id = -1 для текущего диска)
int fs_init(int disk_id);

// Открыть файл (проверка существования)
int fs_open(const char* filename, uint32_t* size);

// Читать файл
int fs_read(const char* filename, void* buffer, size_t size);

// Записать файл (создаёт или перезаписывает)
int fs_write(const char* filename, const void* data, size_t size);

// Список файлов в директории
int fs_list_dir(const char* path, char* buffer, size_t buffer_size);

// Удалить файл
int fs_delete(const char* filename);

// Получить информацию о диске
int fs_get_disk_usage(uint32_t* total_bytes, uint32_t* used_bytes, uint32_t* free_bytes);

// Работа с путями
int fs_resolve_path(const char* path, int* disk_id, char* filename);
int fs_create_dir(const char* path);

#endif