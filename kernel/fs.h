#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>

// ----------------------------------------------
// Файловая система MOS с журналированием и fsck
// ----------------------------------------------

#define FS_MAGIC 0x4D4F5320  // "MOS "
#define FS_MAX_FILES 256
#define FS_FILENAME_LEN 255   // длинные имена
#define FS_SECTOR_SIZE 512

// Флаги записи файла
#define FS_FLAG_FREE      0x00
#define FS_FLAG_OCCUPIED  0x01
#define FS_FLAG_DIRECTORY 0x02
#define FS_FLAG_SYMLINK   0x04   // символическая ссылка (опционально)

// Запись файла (размер ~ 255+4+4+4+1+4 = 272 байта, округляется до 276)
struct fs_file_entry {
    char name[FS_FILENAME_LEN];
    uint32_t start_sector;
    uint32_t sector_count;
    uint32_t size_bytes;
    uint8_t flags;
    uint32_t parent_dir;        // 0xFFFFFFFF = корень
    // можно добавить uid, gid, mode, время и т.д.
} __attribute__((packed));

// Суперблок
struct fs_boot_sector {
    uint32_t magic;
    uint32_t version;
    uint32_t total_sectors;
    uint32_t file_table_sector;
    uint32_t data_start_sector;
    uint32_t file_count;
    uint32_t free_bitmap_sector;
    uint32_t free_bitmap_size;   // размер карты в байтах
    uint32_t log_start_sector;   // сектор начала журнала
    uint32_t log_size;           // размер журнала в секторах
    uint32_t log_next;           // следующий свободный слот в журнале (в секторах)
    uint32_t log_magic;          // маркер корректного завершения журнала (0x4C4F4700)
    uint8_t reserved[FS_SECTOR_SIZE - 48];
} __attribute__((packed));

// Запись журнала (redo-лог)
struct fs_log_entry {
    uint32_t checksum;       // простая контрольная сумма для проверки целостности
    uint32_t sector;         // номер сектора, который был изменён
    uint32_t old_data[128];  // старые данные (512 байт) - храним старые данные для возможного отката (но для redo достаточно новых)
    uint32_t new_data[128];  // новые данные
} __attribute__((packed));

// Публичный интерфейс (не изменился)
int fs_init(int disk_id);
int fs_open(const char* filename, uint32_t* size);
int fs_read(const char* filename, void* buffer, size_t size);
int fs_write(const char* filename, const void* data, size_t size);
int fs_list_dir(const char* path, char* buffer, size_t buffer_size);
int fs_delete(const char* filename);
int fs_get_disk_usage(uint32_t* total_bytes, uint32_t* used_bytes, uint32_t* free_bytes);
int fs_resolve_path(const char* path, int* disk_id, char* filename);
int fs_create_dir(const char* path);

// Дополнительные функции (для утилит)
int fs_check_integrity(void);          // проверка ФС
int fs_recover(void);                 // восстановление из журнала

#endif