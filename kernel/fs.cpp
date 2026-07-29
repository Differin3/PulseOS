#include "fs.h"
#include "drivers/storage/ata.h"
#include "drivers/storage/disk_manager.h"
#include "utils.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

static struct fs_boot_sector boot_sector;
static struct fs_file_entry file_table[FS_MAX_FILES];
static bool fs_initialized = false;
static uint32_t root_dir_index = 0xFFFFFFFF;

// ---- Вспомогательные функции для битовой карты ----

static uint8_t* bitmap = NULL;      // кэш битовой карты (в памяти)
static uint32_t bitmap_sectors = 0; // количество секторов, занятых картой

// Загрузить битовую карту с диска
static int fs_load_bitmap() {
    if (bitmap) return 0;
    bitmap_sectors = (boot_sector.total_sectors + 7) / 8 / FS_SECTOR_SIZE + 1;
    bitmap = (uint8_t*)malloc(bitmap_sectors * FS_SECTOR_SIZE);
    if (!bitmap) return -1;
    return disk_read_sectors(boot_sector.free_bitmap_sector, bitmap_sectors, bitmap);
}

// Сохранить битовую карту на диск
static int fs_save_bitmap() {
    if (!bitmap) return 0;
    return disk_write_sectors(boot_sector.free_bitmap_sector, bitmap_sectors, bitmap);
}

// Проверить, занят ли сектор
static int fs_bitmap_test(uint32_t sector) {
    if (sector >= boot_sector.total_sectors) return 1;
    uint32_t byte_idx = sector / 8;
    uint8_t bit = 1 << (sector % 8);
    return (bitmap[byte_idx] & bit) ? 1 : 0;
}

// Установить сектор как занятый (1) или свободный (0)
static void fs_bitmap_set(uint32_t sector, int occupied) {
    if (sector >= boot_sector.total_sectors) return;
    uint32_t byte_idx = sector / 8;
    uint8_t bit = 1 << (sector % 8);
    if (occupied)
        bitmap[byte_idx] |= bit;
    else
        bitmap[byte_idx] &= ~bit;
}

// Найти непрерывный блок свободных секторов
static int fs_alloc_blocks(uint32_t count, uint32_t* start) {
    if (!bitmap || count == 0) return -1;
    uint32_t free_run = 0;
    for (uint32_t i = boot_sector.data_start_sector; i < boot_sector.total_sectors; i++) {
        if (!fs_bitmap_test(i)) {
            free_run++;
            if (free_run >= count) {
                *start = i - count + 1;
                return 0;
            }
        } else {
            free_run = 0;
        }
    }
    return -1; // нет свободного блока
}

// Освободить блок секторов
static void fs_free_blocks(uint32_t start, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        fs_bitmap_set(start + i, 0);
    }
}

// ---- Работа с таблицей файлов ----

// Сохранить таблицу файлов на диск
static int fs_save_file_table() {
    uint32_t table_sectors = (FS_MAX_FILES * sizeof(struct fs_file_entry) + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    if (disk_write_sectors(boot_sector.file_table_sector, table_sectors, file_table) != 0)
        return -1;
    if (disk_write_sector(0, &boot_sector) != 0)
        return -1;
    return 0;
}

// Найти свободный слот в таблице
static int fs_find_free_slot() {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (file_table[i].flags == FS_FLAG_FREE)
            return i;
    }
    return -1;
}

// Найти файл в директории
static int fs_find_file_in_dir(const char* name, uint32_t parent_idx) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if ((file_table[i].flags & FS_FLAG_OCCUPIED) && file_table[i].parent_dir == parent_idx) {
            if (strcmp(file_table[i].name, name) == 0)
                return i;
        }
    }
    return -1;
}

// Разрешить путь к индексу файла
static int fs_resolve_path_to_index(const char* path) {
    if (!path || path[0] != '/') return -1;
    // упрощённо: ищем последний слеш
    const char* last_slash = strrchr(path, '/');
    if (!last_slash) return -1;
    const char* basename = last_slash + 1;
    if (!basename[0]) return -1;

    // проходим по пути до последнего слеша
    uint32_t current_parent = root_dir_index;
    char dir_name[FS_FILENAME_LEN];
    const char* p = path + 1;
    while (p < last_slash) {
        int i = 0;
        while (p < last_slash && *p != '/' && i < FS_FILENAME_LEN - 1) {
            dir_name[i++] = *p++;
        }
        dir_name[i] = 0;
        if (*p == '/') p++;
        if (dir_name[0] == 0) continue;
        int idx = fs_find_file_in_dir(dir_name, current_parent);
        if (idx < 0 || !(file_table[idx].flags & FS_FLAG_DIRECTORY))
            return -1;
        current_parent = idx;
    }
    return fs_find_file_in_dir(basename, current_parent);
}

// ---- Публичные функции ----

int fs_init(int disk_id) {
    if (disk_id >= 0 && disk_select(disk_id) != 0) return -1;
    if (fs_initialized) return 0;

    if (disk_read_sector(0, &boot_sector) != 0) return -1;

    if (boot_sector.magic != FS_MAGIC) {
        // Создание новой ФС
        boot_sector.magic = FS_MAGIC;
        boot_sector.version = 1;
        boot_sector.total_sectors = disk_get_size_sectors();
        if (boot_sector.total_sectors == 0) boot_sector.total_sectors = 1024;
        boot_sector.file_table_sector = 1;
        uint32_t table_sectors = (FS_MAX_FILES * sizeof(struct fs_file_entry) + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
        boot_sector.data_start_sector = 1 + table_sectors;
        boot_sector.file_count = 0;
        boot_sector.free_bitmap_sector = boot_sector.data_start_sector;
        boot_sector.free_bitmap_size = (boot_sector.total_sectors + 7) / 8;
        uint32_t bitmap_bytes = boot_sector.free_bitmap_size;
        uint32_t bitmap_sectors_needed = (bitmap_bytes + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
        // Смещаем начало данных после битовой карты
        boot_sector.data_start_sector += bitmap_sectors_needed;

        // Очищаем таблицу файлов
        memset(file_table, 0, sizeof(file_table));
        for (int i = 0; i < FS_MAX_FILES; i++) {
            file_table[i].flags = FS_FLAG_FREE;
            file_table[i].parent_dir = 0xFFFFFFFF;
        }
        root_dir_index = 0xFFFFFFFF;

        // Инициализация битовой карты (все секторы свободны)
        uint32_t total_bitmap_sectors = (boot_sector.total_sectors + 7) / 8 / FS_SECTOR_SIZE + 1;
        bitmap = (uint8_t*)malloc(total_bitmap_sectors * FS_SECTOR_SIZE);
        if (!bitmap) return -1;
        memset(bitmap, 0, total_bitmap_sectors * FS_SECTOR_SIZE);
        // Помечаем занятыми секторы суперблока, таблицы, битовой карты и данные
        for (uint32_t i = 0; i < boot_sector.data_start_sector; i++) {
            fs_bitmap_set(i, 1);
        }
        fs_save_bitmap();
        free(bitmap);
        bitmap = NULL;

        // Записываем суперблок и таблицу
        if (disk_write_sector(0, &boot_sector) != 0) return -1;
        if (disk_write_sectors(boot_sector.file_table_sector, table_sectors, file_table) != 0) return -1;

        fs_initialized = true;

        // Создаём базовые каталоги
        fs_create_dir("/bin");
        fs_create_dir("/usr");
        fs_create_dir("/usr/bin");
        fs_create_dir("/etc");
        fs_create_dir("/etc/network");
        fs_create_dir("/dev");
        fs_create_dir("/mnt");
        fs_create_dir("/home");
        fs_create_dir("/tmp");
        fs_create_dir("/www");
        return 0;
    }

    // ФС уже существует – загружаем таблицу
    uint32_t table_sectors = (FS_MAX_FILES * sizeof(struct fs_file_entry) + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    if (disk_read_sectors(boot_sector.file_table_sector, table_sectors, file_table) != 0) return -1;
    fs_initialized = true;

    // Загружаем битовую карту в кэш
    if (fs_load_bitmap() != 0) return -1;

    root_dir_index = 0xFFFFFFFF;
    return 0;
}

int fs_open(const char* filename, uint32_t* size) {
    if (!fs_initialized) return -1;
    int idx = (filename[0] == '/') ? fs_resolve_path_to_index(filename) : fs_find_file_in_dir(filename, root_dir_index);
    if (idx < 0) return -1;
    if (size) *size = file_table[idx].size_bytes;
    return 0;
}

int fs_read(const char* filename, void* buffer, size_t size) {
    if (!fs_initialized) return -1;
    int idx = (filename[0] == '/') ? fs_resolve_path_to_index(filename) : fs_find_file_in_dir(filename, root_dir_index);
    if (idx < 0) return -1;
    size_t read_sz = (size < file_table[idx].size_bytes) ? size : file_table[idx].size_bytes;
    uint32_t sectors = (read_sz + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    if (disk_read_sectors(file_table[idx].start_sector, sectors, buffer) != 0) return -1;
    return (int)read_sz;
}

int fs_write(const char* filename, const void* data, size_t size) {
    if (!fs_initialized) return -1;
    int idx = -1;
    if (filename[0] == '/') {
        idx = fs_resolve_path_to_index(filename);
    } else {
        idx = fs_find_file_in_dir(filename, root_dir_index);
    }

    bool is_new = (idx < 0);
    if (is_new) {
        idx = fs_find_free_slot();
        if (idx < 0) return -1;
        // Извлекаем имя из пути
        const char* name_start = strrchr(filename, '/');
        if (name_start) name_start++;
        else name_start = filename;
        strncpy(file_table[idx].name, name_start, FS_FILENAME_LEN - 1);
        file_table[idx].name[FS_FILENAME_LEN - 1] = 0;
        file_table[idx].flags = FS_FLAG_OCCUPIED;
        // Определяем родительскую директорию
        if (filename[0] == '/') {
            char dir_path[128];
            strcpy(dir_path, filename);
            char* last_slash = strrchr(dir_path, '/');
            if (last_slash) {
                if (last_slash == dir_path) {
                    // корень
                    file_table[idx].parent_dir = root_dir_index;
                } else {
                    *last_slash = 0;
                    // найти индекс директории
                    int dir_idx = fs_resolve_path_to_index(dir_path);
                    if (dir_idx >= 0 && (file_table[dir_idx].flags & FS_FLAG_DIRECTORY))
                        file_table[idx].parent_dir = dir_idx;
                    else
                        file_table[idx].parent_dir = root_dir_index;
                }
            } else {
                file_table[idx].parent_dir = root_dir_index;
            }
        } else {
            // относительный путь – просто корень
            file_table[idx].parent_dir = root_dir_index;
        }
        boot_sector.file_count++;
    }

    // Вычисляем необходимое количество секторов
    uint32_t needed_sectors = (size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    uint32_t old_sectors = file_table[idx].sector_count;

    // Если новый размер больше, выделяем новый блок
    if (needed_sectors > old_sectors) {
        // Освобождаем старые секторы
        if (old_sectors > 0) {
            fs_free_blocks(file_table[idx].start_sector, old_sectors);
        }
        // Выделяем новый блок
        uint32_t new_start = 0;
        if (fs_alloc_blocks(needed_sectors, &new_start) != 0) {
            // не удалось выделить — возвращаем ошибку
            if (is_new) {
                file_table[idx].flags = FS_FLAG_FREE;
                boot_sector.file_count--;
            }
            return -1;
        }
        file_table[idx].start_sector = new_start;
        file_table[idx].sector_count = needed_sectors;
    }

    file_table[idx].size_bytes = size;

    // Запись данных
    if (size > 0) {
        uint8_t* buf = (uint8_t*)malloc(needed_sectors * FS_SECTOR_SIZE);
        if (!buf) return -1;
        memset(buf, 0, needed_sectors * FS_SECTOR_SIZE);
        memcpy(buf, data, size);
        if (disk_write_sectors(file_table[idx].start_sector, needed_sectors, buf) != 0) {
            free(buf);
            if (is_new) {
                file_table[idx].flags = FS_FLAG_FREE;
                boot_sector.file_count--;
            }
            return -1;
        }
        free(buf);
    }

    // Обновляем битовую карту (помечаем занятые секторы)
    for (uint32_t i = 0; i < needed_sectors; i++) {
        fs_bitmap_set(file_table[idx].start_sector + i, 1);
    }

    // Сохраняем изменения
    if (fs_save_bitmap() != 0) return -1;
    if (fs_save_file_table() != 0) return -1;

    return 0;
}

int fs_list_dir(const char* path, char* buffer, size_t buffer_size) {
    if (!fs_initialized) return -1;
    uint32_t parent = root_dir_index;
    if (path[0] != 0 && (path[0] != '/' || path[1] != 0)) {
        // разрешаем путь
        int dir_idx = fs_resolve_path_to_index(path);
        if (dir_idx < 0 || !(file_table[dir_idx].flags & FS_FLAG_DIRECTORY))
            return -1;
        parent = dir_idx;
    }
    size_t pos = 0;
    for (int i = 0; i < FS_MAX_FILES && pos < buffer_size - 1; i++) {
        if ((file_table[i].flags & FS_FLAG_OCCUPIED) && file_table[i].parent_dir == parent) {
            const char* name = file_table[i].name;
            while (*name && pos < buffer_size - 1) {
                buffer[pos++] = *name++;
            }
            if (file_table[i].flags & FS_FLAG_DIRECTORY) {
                if (pos < buffer_size - 1) buffer[pos++] = '/';
            }
            if (pos < buffer_size - 1) buffer[pos++] = '\n';
        }
    }
    buffer[pos] = 0;
    return (int)pos;
}

int fs_delete(const char* filename) {
    if (!fs_initialized) return -1;
    int idx = (filename[0] == '/') ? fs_resolve_path_to_index(filename) : fs_find_file_in_dir(filename, root_dir_index);
    if (idx < 0) return -1;
    if (file_table[idx].flags & FS_FLAG_DIRECTORY) {
        // проверим, что директория пуста
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if ((file_table[i].flags & FS_FLAG_OCCUPIED) && file_table[i].parent_dir == (uint32_t)idx)
                return -1; // не пуста
        }
    }
    // освобождаем секторы
    if (file_table[idx].sector_count > 0) {
        fs_free_blocks(file_table[idx].start_sector, file_table[idx].sector_count);
    }
    file_table[idx].flags = FS_FLAG_FREE;
    file_table[idx].name[0] = 0;
    file_table[idx].sector_count = 0;
    file_table[idx].size_bytes = 0;
    boot_sector.file_count--;
    if (fs_save_bitmap() != 0) return -1;
    if (fs_save_file_table() != 0) return -1;
    return 0;
}

int fs_get_disk_usage(uint32_t* total_bytes, uint32_t* used_bytes, uint32_t* free_bytes) {
    if (!fs_initialized) return -1;
    uint32_t total = boot_sector.total_sectors * FS_SECTOR_SIZE;
    uint32_t used_sec = 0;
    for (uint32_t i = 0; i < boot_sector.total_sectors; i++) {
        if (fs_bitmap_test(i)) used_sec++;
    }
    uint32_t used = used_sec * FS_SECTOR_SIZE;
    uint32_t free = total > used ? total - used : 0;
    if (total_bytes) *total_bytes = total;
    if (used_bytes) *used_bytes = used;
    if (free_bytes) *free_bytes = free;
    return 0;
}

int fs_resolve_path(const char* path, int* disk_id, char* filename) {
    // упрощённо
    if (disk_id) *disk_id = 0;
    const char* p = path;
    if (*p == '/') p++;
    int i = 0;
    while (*p && *p != '/' && i < FS_FILENAME_LEN - 1) {
        filename[i++] = *p++;
    }
    filename[i] = 0;
    return 0;
}

int fs_create_dir(const char* path) {
    if (!fs_initialized) return -1;
    // разбираем путь и создаём все недостающие директории
    char dir_copy[128];
    strcpy(dir_copy, path);
    char* token = strtok(dir_copy, "/");
    uint32_t current_parent = root_dir_index;
    while (token) {
        int idx = fs_find_file_in_dir(token, current_parent);
        if (idx < 0) {
            idx = fs_find_free_slot();
            if (idx < 0) return -1;
            strncpy(file_table[idx].name, token, FS_FILENAME_LEN - 1);
            file_table[idx].name[FS_FILENAME_LEN - 1] = 0;
            file_table[idx].flags = FS_FLAG_OCCUPIED | FS_FLAG_DIRECTORY;
            file_table[idx].parent_dir = current_parent;
            file_table[idx].start_sector = 0;
            file_table[idx].sector_count = 0;
            file_table[idx].size_bytes = 0;
            boot_sector.file_count++;
            // сохраняем таблицу
            if (fs_save_file_table() != 0) return -1;
            current_parent = idx;
        } else {
            if (!(file_table[idx].flags & FS_FLAG_DIRECTORY)) return -1;
            current_parent = idx;
        }
        token = strtok(NULL, "/");
    }
    return 0;
}