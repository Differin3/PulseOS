#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>

// Простая файловая система
// Структура: загрузочный сектор + таблица файлов + данные

#define FS_MAGIC 0x4D4F5320  // "MOS " в ASCII
#define FS_MAX_FILES 64
#define FS_FILENAME_LEN 32
#define FS_SECTOR_SIZE 512

// Флаги для fs_file_entry
#define FS_FLAG_FREE      0x00
#define FS_FLAG_OCCUPIED  0x01
#define FS_FLAG_DIRECTORY 0x02

// Структура записи файла
struct fs_file_entry {
    char name[FS_FILENAME_LEN];
    uint32_t start_sector;
    uint32_t size_bytes;
    uint8_t flags;  // биты: 0=свободен, 1=занят, 2=директория
    uint32_t parent_dir;  // индекс родительской директории (0xFFFFFFFF для корня)
} __attribute__((packed));

// Загрузочный сектор файловой системы
struct fs_boot_sector {
    uint32_t magic;
    uint32_t version;
    uint32_t total_sectors;
    uint32_t file_table_sector;
    uint32_t data_start_sector;
    uint32_t file_count;
    uint8_t reserved[FS_SECTOR_SIZE - 24];
} __attribute__((packed));

// Инициализация файловой системы (disk_id = -1 для текущего диска)
int fs_init(int disk_id);

// Открыть файл
int fs_open(const char* filename, uint32_t* size);

// Читать файл
int fs_read(const char* filename, void* buffer, size_t size);

// Записать файл
int fs_write(const char* filename, const void* data, size_t size);

// Список файлов
int fs_list(char* buffer, size_t buffer_size);

// Удалить файл
int fs_delete(const char* filename);

// Получить информацию о емкости диска
int fs_get_disk_usage(uint32_t* total_bytes, uint32_t* used_bytes, uint32_t* free_bytes);

// Работа с путями и директориями
int fs_resolve_path(const char* path, int* disk_id, char* filename);
int fs_create_dir(const char* path);
int fs_list_dir(const char* path, char* buffer, size_t buffer_size);

#endif

