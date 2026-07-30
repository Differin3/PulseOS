#include "fs.h"
#include "drivers/storage/ata.h"
#include "drivers/storage/disk_manager.h"
#include "utils.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

static struct fs_boot_sector boot_sector;
static struct fs_file_entry file_table[FS_MAX_FILES];
static bool fs_initialized = false;
static uint32_t root_dir_index = 0xFFFFFFFF;
static uint8_t* bitmap = NULL;
static uint32_t bitmap_sectors = 0;
static bool fs_recovery_attempted = false;

// ---- Контрольная сумма (простейшая) ----
static uint32_t fs_checksum(const void* data, size_t len) {
    uint32_t sum = 0;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        sum += p[i];
        sum = (sum << 1) | (sum >> 31); // циклический сдвиг
    }
    return sum;
}

// ---- Журналирование ----

// Записать в журнал изменение сектора (redo-лог)
static int fs_log_write(uint32_t sector, const void* old_data, const void* new_data) {
    if (boot_sector.log_size == 0 || boot_sector.log_start_sector == 0) return 0; // лог не включён

    // Определяем запись в журнале
    uint32_t entry_size = sizeof(struct fs_log_entry);
    uint32_t entries_per_sector = FS_SECTOR_SIZE / entry_size;
    uint32_t current_slot = boot_sector.log_next;
    uint32_t current_sector = boot_sector.log_start_sector + (current_slot / entries_per_sector);
    uint32_t offset_in_sector = (current_slot % entries_per_sector) * entry_size;

    // Если журнал заполнен, зацикливаем (но проще просто сбросить лог и пометить как старый)
    if (current_sector >= boot_sector.log_start_sector + boot_sector.log_size) {
        // перезаписываем с начала (старый лог теряется)
        boot_sector.log_next = 0;
        current_slot = 0;
        current_sector = boot_sector.log_start_sector;
        offset_in_sector = 0;
    }

    struct fs_log_entry entry;
    entry.checksum = 0;
    entry.sector = sector;
    memcpy(entry.old_data, old_data, FS_SECTOR_SIZE);
    memcpy(entry.new_data, new_data, FS_SECTOR_SIZE);
    entry.checksum = fs_checksum(&entry, sizeof(entry) - sizeof(uint32_t));

    // Читаем сектор, куда будем писать
    uint8_t sector_buf[FS_SECTOR_SIZE];
    if (disk_read_sector(current_sector, sector_buf) != 0) return -1;
    memcpy(sector_buf + offset_in_sector, &entry, entry_size);
    if (disk_write_sector(current_sector, sector_buf) != 0) return -1;

    // Обновляем указатель следующей записи
    boot_sector.log_next++;
    disk_write_sector(0, &boot_sector); // обновляем суперблок
    return 0;
}

// Применить журнал (восстановление)
int fs_recover(void) {
    if (boot_sector.log_size == 0) return 0;
    uint32_t entry_size = sizeof(struct fs_log_entry);
    uint32_t entries_per_sector = FS_SECTOR_SIZE / entry_size;
    uint32_t total_entries = boot_sector.log_size * entries_per_sector;
    uint32_t last_written = boot_sector.log_next;
    if (last_written > total_entries) last_written = total_entries;

    uint8_t sector_buf[FS_SECTOR_SIZE];
    for (uint32_t i = 0; i < last_written; i++) {
        uint32_t sec = boot_sector.log_start_sector + (i / entries_per_sector);
        uint32_t off = (i % entries_per_sector) * entry_size;
        if (disk_read_sector(sec, sector_buf) != 0) continue;
        struct fs_log_entry* entry = (struct fs_log_entry*)(sector_buf + off);
        uint32_t check = fs_checksum(entry, sizeof(*entry) - sizeof(uint32_t));
        if (check != entry->checksum) continue; // повреждённая запись
        // Применяем новые данные к сектору
        if (disk_write_sector(entry->sector, (void*)entry->new_data) != 0) continue;
        // Помечаем битовую карту (если сектор был освобождён или занят)
        // Не обновляем битовую карту здесь, т.к. она будет загружена позже
    }
    // Очищаем лог после восстановления
    boot_sector.log_next = 0;
    disk_write_sector(0, &boot_sector);
    return 0;
}

// ---- Битовая карта ----

static int fs_load_bitmap() {
    if (bitmap) return 0;
    bitmap_sectors = (boot_sector.total_sectors + 7) / 8 / FS_SECTOR_SIZE + 1;
    if (boot_sector.free_bitmap_size > 0) {
        bitmap_sectors = (boot_sector.free_bitmap_size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    }
    bitmap = (uint8_t*)malloc(bitmap_sectors * FS_SECTOR_SIZE);
    if (!bitmap) return -1;
    return disk_read_sectors(boot_sector.free_bitmap_sector, bitmap_sectors, bitmap);
}

static int fs_save_bitmap() {
    if (!bitmap) return 0;
    return disk_write_sectors(boot_sector.free_bitmap_sector, bitmap_sectors, bitmap);
}

static int fs_bitmap_test(uint32_t sector) {
    if (sector >= boot_sector.total_sectors) return 1;
    uint32_t byte_idx = sector / 8;
    uint8_t bit = 1 << (sector % 8);
    return (bitmap[byte_idx] & bit) ? 1 : 0;
}

static void fs_bitmap_set(uint32_t sector, int occupied) {
    if (sector >= boot_sector.total_sectors) return;
    uint32_t byte_idx = sector / 8;
    uint8_t bit = 1 << (sector % 8);
    if (occupied)
        bitmap[byte_idx] |= bit;
    else
        bitmap[byte_idx] &= ~bit;
}

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
    return -1;
}

static void fs_free_blocks(uint32_t start, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        fs_bitmap_set(start + i, 0);
    }
}

// ---- Проверка целостности (fsck) ----

int fs_check_integrity(void) {
    if (!fs_initialized) return -1;
    int errors = 0;
    // Проверяем битовую карту: все занятые секторы должны быть отмечены в карте
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (file_table[i].flags & FS_FLAG_OCCUPIED) {
            uint32_t start = file_table[i].start_sector;
            uint32_t count = file_table[i].sector_count;
            for (uint32_t j = 0; j < count; j++) {
                uint32_t sec = start + j;
                if (!fs_bitmap_test(sec)) {
                    errors++;
                    // исправляем: помечаем как занятый
                    fs_bitmap_set(sec, 1);
                }
            }
        }
    }
    // Проверяем, что все занятые секторы принадлежат какому-то файлу
    for (uint32_t i = boot_sector.data_start_sector; i < boot_sector.total_sectors; i++) {
        if (fs_bitmap_test(i)) {
            bool found = false;
            for (int j = 0; j < FS_MAX_FILES; j++) {
                if (file_table[j].flags & FS_FLAG_OCCUPIED) {
                    uint32_t start = file_table[j].start_sector;
                    uint32_t count = file_table[j].sector_count;
                    if (i >= start && i < start + count) {
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                errors++;
                fs_bitmap_set(i, 0); // освобождаем "потерянный" сектор
            }
        }
    }
    if (errors > 0) {
        // сохраняем исправленную карту
        fs_save_bitmap();
    }
    return errors;
}

// ---- Таблица файлов ----

static int fs_save_file_table() {
    uint32_t table_sectors = (FS_MAX_FILES * sizeof(struct fs_file_entry) + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    // Сохраняем старые данные сектора таблицы для журнала
    uint8_t old_table[FS_SECTOR_SIZE * 8]; // максимум 8 секторов
    uint8_t new_table[FS_SECTOR_SIZE * 8];
    memset(old_table, 0, sizeof(old_table));
    memset(new_table, 0, sizeof(new_table));
    // читаем старые данные (если есть)
    disk_read_sectors(boot_sector.file_table_sector, table_sectors, old_table);
    // копируем новую таблицу
    memcpy(new_table, file_table, FS_MAX_FILES * sizeof(struct fs_file_entry));
    // записываем в журнал каждый сектор отдельно
    for (uint32_t i = 0; i < table_sectors; i++) {
        fs_log_write(boot_sector.file_table_sector + i,
                     old_table + i * FS_SECTOR_SIZE,
                     new_table + i * FS_SECTOR_SIZE);
    }
    // записываем на диск
    if (disk_write_sectors(boot_sector.file_table_sector, table_sectors, file_table) != 0) return -1;
    if (disk_write_sector(0, &boot_sector) != 0) return -1;
    return 0;
}

static int fs_find_free_slot() {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (file_table[i].flags == FS_FLAG_FREE)
            return i;
    }
    return -1;
}

static int fs_find_file_in_dir(const char* name, uint32_t parent_idx) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if ((file_table[i].flags & FS_FLAG_OCCUPIED) && file_table[i].parent_dir == parent_idx) {
            if (strcmp(file_table[i].name, name) == 0)
                return i;
        }
    }
    return -1;
}

static int fs_resolve_path_to_index(const char* path) {
    if (!path || path[0] != '/') return -1;
    const char* last_slash = strrchr(path, '/');
    if (!last_slash) return -1;
    const char* basename = last_slash + 1;
    if (!basename[0]) return -1;

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
        boot_sector.data_start_sector += bitmap_sectors_needed;

        // Журнал выделяем после данных
        uint32_t log_sectors = 16; // 16 секторов ~ 8 КБ для журнала (достаточно для многих операций)
        boot_sector.log_start_sector = boot_sector.data_start_sector;
        boot_sector.log_size = log_sectors;
        boot_sector.log_next = 0;
        boot_sector.log_magic = 0x4C4F4700;

        // Очищаем таблицу
        memset(file_table, 0, sizeof(file_table));
        for (int i = 0; i < FS_MAX_FILES; i++) {
            file_table[i].flags = FS_FLAG_FREE;
            file_table[i].parent_dir = 0xFFFFFFFF;
        }
        root_dir_index = 0xFFFFFFFF;

        // Инициализируем битовую карту
        bitmap_sectors = (boot_sector.total_sectors + 7) / 8 / FS_SECTOR_SIZE + 1;
        bitmap = (uint8_t*)malloc(bitmap_sectors * FS_SECTOR_SIZE);
        if (!bitmap) return -1;
        memset(bitmap, 0, bitmap_sectors * FS_SECTOR_SIZE);
        for (uint32_t i = 0; i < boot_sector.data_start_sector + log_sectors; i++) {
            fs_bitmap_set(i, 1);
        }
        fs_save_bitmap();
        free(bitmap);
        bitmap = NULL;

        // Запись суперблока и таблицы
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

    // Загружаем битовую карту
    if (fs_load_bitmap() != 0) return -1;

    // Восстановление из журнала (если есть записи)
    if (boot_sector.log_next > 0 && !fs_recovery_attempted) {
        fs_recovery_attempted = true;
        fs_recover();
    }

    // Проверка целостности (fsck)
    int errors = fs_check_integrity();
    if (errors > 0) {
        // можно вывести предупреждение
    }

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
        const char* name_start = strrchr(filename, '/');
        if (name_start) name_start++;
        else name_start = filename;
        strncpy(file_table[idx].name, name_start, FS_FILENAME_LEN - 1);
        file_table[idx].name[FS_FILENAME_LEN - 1] = 0;
        file_table[idx].flags = FS_FLAG_OCCUPIED;
        // Определяем родителя
        if (filename[0] == '/') {
            char dir_path[128];
            strcpy(dir_path, filename);
            char* last_slash = strrchr(dir_path, '/');
            if (last_slash) {
                if (last_slash == dir_path) {
                    file_table[idx].parent_dir = root_dir_index;
                } else {
                    *last_slash = 0;
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
            file_table[idx].parent_dir = root_dir_index;
        }
        boot_sector.file_count++;
    }

    uint32_t needed_sectors = (size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    uint32_t old_sectors = file_table[idx].sector_count;

    if (needed_sectors > old_sectors) {
        if (old_sectors > 0) {
            fs_free_blocks(file_table[idx].start_sector, old_sectors);
        }
        uint32_t new_start = 0;
        if (fs_alloc_blocks(needed_sectors, &new_start) != 0) {
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
        // Сохраняем старые данные секторов для журнала
        uint8_t old_data[FS_SECTOR_SIZE];
        for (uint32_t s = 0; s < needed_sectors; s++) {
            disk_read_sector(file_table[idx].start_sector + s, old_data);
            fs_log_write(file_table[idx].start_sector + s, old_data, buf + s * FS_SECTOR_SIZE);
        }
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

    // Обновляем битовую карту
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
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if ((file_table[i].flags & FS_FLAG_OCCUPIED) && file_table[i].parent_dir == (uint32_t)idx)
                return -1;
        }
    }
    // освобождаем секторы
    if (file_table[idx].sector_count > 0) {
        // Записываем в журнал освобождение секторов
        uint8_t zero_sector[FS_SECTOR_SIZE];
        memset(zero_sector, 0, FS_SECTOR_SIZE);
        uint8_t old_data[FS_SECTOR_SIZE];
        for (uint32_t s = 0; s < file_table[idx].sector_count; s++) {
            disk_read_sector(file_table[idx].start_sector + s, old_data);
            fs_log_write(file_table[idx].start_sector + s, old_data, zero_sector);
        }
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