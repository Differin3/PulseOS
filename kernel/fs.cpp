#include "fs.h"
#include "drivers/storage/ata.h"
#include "drivers/storage/disk_manager.h"
#include "utils.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>


static struct fs_boot_sector boot_sector;
static struct fs_file_entry file_table[FS_MAX_FILES];
static bool fs_initialized = false;
static uint32_t root_dir_index = 0xFFFFFFFF; // Индекс корневой директории

// Инициализация файловой системы
int fs_init(int disk_id) {
    if (disk_id >= 0 && disk_select(disk_id) != 0) {
        return -1;
    }
    if (fs_initialized) return 0;
    
    // Проверяем что диск инициализирован (пропускаем если нет диска)
    // Читаем загрузочный сектор
    if (disk_read_sector(0, &boot_sector) != 0) {
        // Если не удалось прочитать, создаем новую ФС
        // Но только если это не ошибка отсутствия диска
        return -1;
    }
    
    // Проверяем magic number
    if (boot_sector.magic != FS_MAGIC) {
        // Файловая система не инициализирована - создаем новую
        boot_sector.magic = FS_MAGIC;
        boot_sector.version = 1;
        
        // Получаем реальный размер диска
        uint32_t disk_size = disk_get_size_sectors();
        if (disk_size == 0) {
            disk_size = 1024; // Fallback если не удалось определить
        }
        boot_sector.total_sectors = disk_size;
        
        boot_sector.file_table_sector = 1;
        boot_sector.data_start_sector = 1 + (FS_MAX_FILES * sizeof(struct fs_file_entry) + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
        boot_sector.file_count = 0;
        
        // Очищаем таблицу файлов
        for (int i = 0; i < FS_MAX_FILES; i++) {
            file_table[i].flags = 0;
            file_table[i].name[0] = 0;
            file_table[i].parent_dir = 0xFFFFFFFF;
        }
        
        // Создаем корневую директорию (виртуальная, индекс 0xFFFFFFFF)
        root_dir_index = 0xFFFFFFFF;
        
        // Записываем загрузочный сектор
        if (disk_write_sector(0, &boot_sector) != 0) {
            return -1;
        }
        
        // Записываем таблицу файлов
        uint32_t table_sectors = (FS_MAX_FILES * sizeof(struct fs_file_entry) + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
        if (disk_write_sectors(boot_sector.file_table_sector, table_sectors, file_table) != 0) {
            return -1;
        }
        
        // Устанавливаем флаг инициализации перед созданием директорий
        fs_initialized = true;
        
        // Создаем базовую структуру директорий
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
    } else {
        // Файловая система уже существует - обновляем размер диска если он неправильный
        uint32_t real_disk_size = disk_get_size_sectors();
        if (real_disk_size > 0 && boot_sector.total_sectors != real_disk_size) {
            boot_sector.total_sectors = real_disk_size;
            // Сохраняем обновленный загрузочный сектор
            disk_write_sector(0, &boot_sector);
        }
        
        // Читаем таблицу файлов
        uint32_t table_sectors = (FS_MAX_FILES * sizeof(struct fs_file_entry) + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
        if (disk_read_sectors(boot_sector.file_table_sector, table_sectors, file_table) != 0) {
            return -1;
        }
        
        // Устанавливаем флаг инициализации
        fs_initialized = true;
        
        // Проверяем наличие базовых директорий, если их нет - создаем
        char test_buf[256];
        if (fs_list_dir("/", test_buf, sizeof(test_buf)) >= 0 && test_buf[0] != 0) {
            // Директории есть, проверяем наличие основных
            bool has_bin = false;
            const char* p = test_buf;
            while (*p) {
                if (p[0] == 'b' && p[1] == 'i' && p[2] == 'n' && (p[3] == '/' || p[3] == '\n' || p[3] == 0)) {
                    has_bin = true;
                    break;
                }
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
            }
            
            // Если основных директорий нет, создаем их
            if (!has_bin) {
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
            }
        } else {
            // Директорий нет вообще, создаем базовую структуру
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
        }
    }
    
    if (!fs_initialized) {
        fs_initialized = true;
    }
    return 0;
}

// Найти файл в таблице (в указанной директории)
static int fs_find_file_in_dir(const char* filename, uint32_t parent_dir) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (file_table[i].flags & FS_FLAG_OCCUPIED) {
            if (file_table[i].parent_dir == parent_dir) {
                int j = 0;
                while (filename[j] && file_table[i].name[j] && filename[j] == file_table[i].name[j]) j++;
                if (!filename[j] && !file_table[i].name[j]) {
                    return i;
                }
            }
        }
    }
    return -1;
}

// Найти файл в таблице (старая версия для обратной совместимости)
static int fs_find_file(const char* filename) {
    return fs_find_file_in_dir(filename, root_dir_index);
}

// Разрешить абсолютный путь к индексу файла (не директории)
static int fs_resolve_path_to_index(const char* path) {
    if (!path || path[0] != '/') return -1;

    const char* last_slash = path;
    const char* p = path;
    while (*p) {
        if (*p == '/') last_slash = p;
        p++;
    }

    const char* basename = (last_slash == path) ? path + 1 : last_slash + 1;
    if (!basename[0]) return -1;

    uint32_t current_parent = root_dir_index;
    const char* path_ptr = path + 1;
    const char* dir_end = last_slash;

    while (path_ptr < dir_end) {
        char dirname[FS_FILENAME_LEN];
        int i = 0;
        while (path_ptr < dir_end && *path_ptr != '/' && i < FS_FILENAME_LEN - 1) {
            dirname[i++] = *path_ptr++;
        }
        dirname[i] = 0;
        if (*path_ptr == '/') path_ptr++;
        if (dirname[0] == 0) continue;

        int dir_idx = fs_find_file_in_dir(dirname, current_parent);
        if (dir_idx < 0 || !(file_table[dir_idx].flags & FS_FLAG_DIRECTORY)) {
            return -1;
        }
        current_parent = (uint32_t)dir_idx;
    }

    return fs_find_file_in_dir(basename, current_parent);
}

// Найти свободный слот
static int fs_find_free_slot() {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (file_table[i].flags == 0) {
            return i;
        }
    }
    return -1;
}

// Сохранить таблицу файлов
static int fs_save_file_table() {
    uint32_t table_sectors = (FS_MAX_FILES * sizeof(struct fs_file_entry) + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    if (disk_write_sectors(boot_sector.file_table_sector, table_sectors, file_table) != 0) {
        return -1;
    }
    if (disk_write_sector(0, &boot_sector) != 0) {
        return -1;
    }
    return 0;
}

// Открыть файл
int fs_open(const char* filename, uint32_t* size) {
    if (!fs_initialized) return -1;

    int idx = -1;
    if (filename[0] == '/') {
        idx = fs_resolve_path_to_index(filename);
    } else {
        idx = fs_find_file(filename);
    }
    if (idx < 0) return -1;

    if (size) *size = file_table[idx].size_bytes;
    return 0;
}

// Читать файл
int fs_read(const char* filename, void* buffer, size_t size) {
    if (!fs_initialized) return -1;

    int idx = -1;
    if (filename[0] == '/') {
        idx = fs_resolve_path_to_index(filename);
    } else {
        idx = fs_find_file(filename);
    }
    if (idx < 0) return -1;
    
    size_t read_size = size < file_table[idx].size_bytes ? size : file_table[idx].size_bytes;
    uint32_t sectors = (read_size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    
    if (disk_read_sectors(file_table[idx].start_sector, sectors, buffer) != 0) {
        return -1;
    }
    
    return read_size;
}

// Записать файл (с поддержкой путей)
int fs_write(const char* filename, const void* data, size_t size) {
    if (!fs_initialized) return -1;
    
    // Если путь начинается с /, используем разрешение пути
    int disk_id;
    char resolved_name[FS_FILENAME_LEN];
    const char* actual_filename = filename;
    
    if (filename[0] == '/') {
        if (fs_resolve_path(filename, &disk_id, resolved_name) != 0) {
            return -1;
        }
        actual_filename = resolved_name;
    }
    
    int idx = -1;
    if (filename[0] == '/') {
        idx = fs_resolve_path_to_index(filename);
    } else {
        idx = fs_find_file(actual_filename);
    }
    bool is_new_file = (idx < 0);
    
    if (is_new_file) {
        // Создаем новый файл
        idx = fs_find_free_slot();
        if (idx < 0) return -1;
        
        // Извлекаем только имя файла (без пути)
        const char* name_start = filename;
        const char* last_slash = filename;
        const char* p = filename;
        while (*p) {
            if (*p == '/') last_slash = p;
            p++;
        }
        if (last_slash != filename || (last_slash == filename && filename[0] == '/')) {
            name_start = last_slash + 1;
        }
        
        // Копируем только имя файла
        int i = 0;
        while (name_start[i] && i < FS_FILENAME_LEN - 1) {
            file_table[idx].name[i] = name_start[i];
            i++;
        }
        file_table[idx].name[i] = 0;
        
        // Находим свободный сектор для данных
        uint32_t max_sector = 0;
        for (int j = 0; j < FS_MAX_FILES; j++) {
            if (file_table[j].flags & FS_FLAG_OCCUPIED && !(file_table[j].flags & FS_FLAG_DIRECTORY)) {
                uint32_t end_sector = file_table[j].start_sector + (file_table[j].size_bytes + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
                if (end_sector > max_sector) max_sector = end_sector;
            }
        }
        file_table[idx].start_sector = max_sector > boot_sector.data_start_sector ? max_sector : boot_sector.data_start_sector;
        
        // Увеличиваем счетчик файлов только для новых файлов
        boot_sector.file_count++;
    }
    
    file_table[idx].size_bytes = size;
    file_table[idx].flags = FS_FLAG_OCCUPIED;
    
    // Определяем родительскую директорию из пути
    if (filename[0] == '/') {
        // Ищем последний / в пути
        const char* last_slash = filename;
        const char* p = filename;
        while (*p) {
            if (*p == '/') last_slash = p;
            p++;
        }
        
        if (last_slash == filename) {
            // Файл в корне
            file_table[idx].parent_dir = root_dir_index;
        } else {
            // Извлекаем путь к директории (без последнего /)
            char dir_path[FS_FILENAME_LEN];
            int dir_path_len = last_slash - filename;
            if (dir_path_len >= FS_FILENAME_LEN) dir_path_len = FS_FILENAME_LEN - 1;
            for (int i = 0; i < dir_path_len; i++) {
                dir_path[i] = filename[i];
            }
            if (dir_path_len == 0) {
                dir_path[0] = '/';
                dir_path[1] = 0;
            } else {
                dir_path[dir_path_len] = 0;
            }
            
            // Находим индекс директории, проходя по пути
            uint32_t current_parent = root_dir_index;
            const char* path_ptr = dir_path;
            if (*path_ptr == '/') path_ptr++; // Пропускаем начальный /
            
            bool path_valid = true;
            while (*path_ptr && path_valid) {
                // Извлекаем имя следующей части пути
                char dirname[FS_FILENAME_LEN];
                int i = 0;
                while (*path_ptr && *path_ptr != '/' && i < FS_FILENAME_LEN - 1) {
                    dirname[i++] = *path_ptr++;
                }
                dirname[i] = 0;
                
                // Пропускаем /
                if (*path_ptr == '/') path_ptr++;
                
                // Если имя пустое, пропускаем
                if (dirname[0] == 0) continue;
                
                // Ищем директорию
                int dir_idx = fs_find_file_in_dir(dirname, current_parent);
                if (dir_idx >= 0 && (file_table[dir_idx].flags & FS_FLAG_DIRECTORY)) {
                    current_parent = dir_idx;
                } else {
                    // Директория не найдена - путь невалиден
                    path_valid = false;
                    break;
                }
            }
            
            // Используем найденную директорию или корень если путь невалиден
            file_table[idx].parent_dir = path_valid ? current_parent : root_dir_index;
        }
    } else {
        // Относительный путь - используем текущую директорию пользователя
        const char* cwd = utils_get_current_directory(); // получаем текущую директорию
        if (cwd && cwd[0] == '/') { // если путь абсолютный
            // Находим индекс директории по пути
            uint32_t current_parent = root_dir_index;
            const char* path_ptr = cwd;
            if (*path_ptr == '/') path_ptr++; // пропускаем начальный /
            
            bool path_valid = true;
            while (*path_ptr && path_valid) {
                // Извлекаем имя следующей части пути
                char dirname[FS_FILENAME_LEN];
                int i = 0;
                while (*path_ptr && *path_ptr != '/' && i < FS_FILENAME_LEN - 1) {
                    dirname[i++] = *path_ptr++;
                }
                dirname[i] = 0;
                
                if (*path_ptr == '/') path_ptr++; // пропускаем /
                if (dirname[0] == 0) continue; // пустое имя пропускаем
                
                // Ищем директорию
                int dir_idx = fs_find_file_in_dir(dirname, current_parent);
                if (dir_idx >= 0 && (file_table[dir_idx].flags & FS_FLAG_DIRECTORY)) {
                    current_parent = dir_idx; // переходим в найденную директорию
                } else {
                    path_valid = false; // директория не найдена
                    break;
                }
            }
            file_table[idx].parent_dir = path_valid ? current_parent : root_dir_index; // используем найденную или корень
        } else {
            file_table[idx].parent_dir = root_dir_index; // fallback на корень
        }
    }
    
    uint32_t sectors = (size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    if (size > 0 && sectors > 0) {
        for (uint32_t s = 0; s < sectors; s++) {
            uint8_t sec[FS_SECTOR_SIZE];
            size_t off = (size_t)s * FS_SECTOR_SIZE;
            for (size_t i = 0; i < FS_SECTOR_SIZE; i++) {
                size_t idx_b = off + i;
                sec[i] = (idx_b < size) ? ((const uint8_t*)data)[idx_b] : 0;
            }
            if (disk_write_sectors(file_table[idx].start_sector + s, 1, sec) != 0) {
                if (is_new_file) {
                    boot_sector.file_count--;
                    file_table[idx].flags = 0;
                    file_table[idx].name[0] = 0;
                }
                return -1;
            }
        }
    }
    
    // Сохраняем таблицу файлов на диск
    if (fs_save_file_table() != 0) {
        return -1;
    }
    
    return 0;
}

// Список файлов (старая версия для обратной совместимости)
int fs_list(char* buffer, size_t buffer_size) {
    return fs_list_dir("/", buffer, buffer_size);
}

// Список файлов в директории
int fs_list_dir(const char* path, char* buffer, size_t buffer_size) {
    if (!fs_initialized) return -1;
    
    uint32_t dir_index = root_dir_index;
    
    // Если путь не корень, находим директорию
    if (path[0] != 0 && (path[0] != '/' || path[1] != 0)) {
        // Проходим по пути и находим директорию
        uint32_t current_parent = root_dir_index;
        const char* path_ptr = path;
        if (*path_ptr == '/') path_ptr++; // Пропускаем начальный /
        
        bool path_valid = true;
        while (*path_ptr && path_valid) {
            // Извлекаем имя следующей части пути
        char dirname[FS_FILENAME_LEN];
            int i = 0;
            while (*path_ptr && *path_ptr != '/' && i < FS_FILENAME_LEN - 1) {
                dirname[i++] = *path_ptr++;
            }
            dirname[i] = 0;
            
            // Пропускаем /
            if (*path_ptr == '/') path_ptr++;
            
            // Если имя пустое, пропускаем
            if (dirname[0] == 0) continue;
            
            // Ищем директорию
            int dir_idx = fs_find_file_in_dir(dirname, current_parent);
            if (dir_idx >= 0 && (file_table[dir_idx].flags & FS_FLAG_DIRECTORY)) {
                current_parent = dir_idx;
            } else {
                // Директория не найдена
                path_valid = false;
                break;
            }
        }
        
        if (path_valid) {
            dir_index = current_parent;
        } else {
            return -1; // Директория не найдена
        }
    }
    
    size_t pos = 0;
    for (int i = 0; i < FS_MAX_FILES && pos < buffer_size - 1; i++) {
        if (file_table[i].flags & FS_FLAG_OCCUPIED) {
            if (file_table[i].parent_dir == dir_index) {
                int j = 0;
                while (file_table[i].name[j] && pos < buffer_size - 1) {
                    buffer[pos++] = file_table[i].name[j++];
                }
                // Добавляем маркер типа
                if (pos < buffer_size - 1) {
                    if (file_table[i].flags & FS_FLAG_DIRECTORY) {
                        buffer[pos++] = '/';
                    }
                }
                if (pos < buffer_size - 1) buffer[pos++] = '\n';
            }
        }
    }
    buffer[pos] = 0;
    return pos;
}

// Удалить файл
int fs_delete(const char* filename) {
    if (!fs_initialized) return -1;

    int idx = -1;
    if (filename[0] == '/') {
        idx = fs_resolve_path_to_index(filename);
    } else {
        idx = fs_find_file(filename);
    }
    if (idx < 0) return -1;
    
    file_table[idx].flags = 0;
    file_table[idx].name[0] = 0;
    boot_sector.file_count--;
    return fs_save_file_table();
}

// Получить информацию о емкости диска
int fs_get_disk_usage(uint32_t* total_bytes, uint32_t* used_bytes, uint32_t* free_bytes) {
    if (!fs_initialized) return -1;
    
    // Общий размер диска в байтах
    uint32_t total = boot_sector.total_sectors * FS_SECTOR_SIZE;
    
    // Занятое место: метаданные (загрузочный сектор + таблица файлов) + данные файлов
    uint32_t table_sectors = (FS_MAX_FILES * sizeof(struct fs_file_entry) + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    uint32_t metadata_bytes = (1 + table_sectors) * FS_SECTOR_SIZE; // Загрузочный сектор + таблица
    
    // Находим последний занятый сектор
    uint32_t last_sector = boot_sector.data_start_sector;
    uint32_t total_file_bytes = 0;
    
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (file_table[i].flags & FS_FLAG_OCCUPIED && !(file_table[i].flags & FS_FLAG_DIRECTORY)) {
            total_file_bytes += file_table[i].size_bytes;
            uint32_t file_end_sector = file_table[i].start_sector + 
                (file_table[i].size_bytes + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
            if (file_end_sector > last_sector) {
                last_sector = file_end_sector;
            }
        }
    }
    
    // Занятое место = метаданные + все секторы до последнего занятого
    uint32_t used = metadata_bytes + (last_sector - boot_sector.data_start_sector) * FS_SECTOR_SIZE;
    
    // Свободное место
    uint32_t free = total > used ? total - used : 0;
    
    if (total_bytes) *total_bytes = total;
    if (used_bytes) *used_bytes = used;
    if (free_bytes) *free_bytes = free;
    
    return 0;
}

// Разрешение пути (упрощенная версия, пока без поддержки нескольких дисков)
int fs_resolve_path(const char* path, int* disk_id, char* filename) {
    if (!fs_initialized) return -1;
    
    // Пока всегда используем диск 0
    if (disk_id) *disk_id = 0;
    
    // Пропускаем начальный /
    const char* p = path;
    if (*p == '/') p++;
    
    // Копируем имя файла/директории
    int i = 0;
    while (*p && *p != '/' && i < FS_FILENAME_LEN - 1) {
        filename[i++] = *p++;
    }
    filename[i] = 0;
    
    // Если путь закончился, возвращаем имя
    if (*p == 0 || (*p == '/' && *(p+1) == 0)) {
        return 0;
    }
    
    // Пока не поддерживаем вложенные пути
    return 0;
}

// Создать директорию (поддержка вложенных путей)
int fs_create_dir(const char* path) {
    if (!fs_initialized) return -1;
    
    // Пропускаем начальный /
    const char* p = path;
    if (*p == '/') p++;
    
    uint32_t current_parent = root_dir_index;
    
    // Обрабатываем путь по частям
    while (*p) {
        // Извлекаем имя следующей части пути
        char dirname[FS_FILENAME_LEN];
        int i = 0;
        while (*p && *p != '/' && i < FS_FILENAME_LEN - 1) {
            dirname[i++] = *p++;
        }
        dirname[i] = 0;
        
        // Пропускаем /
        if (*p == '/') p++;
        
        // Если имя пустое, пропускаем
        if (dirname[0] == 0) continue;
        
        // Проверяем, существует ли уже
        int idx = fs_find_file_in_dir(dirname, current_parent);
        if (idx >= 0) {
            // Уже существует
            if (file_table[idx].flags & FS_FLAG_DIRECTORY) {
                current_parent = idx; // Переходим в существующую директорию
                continue;
            }
            return -1; // Файл с таким именем существует
        }
        
        // Создаем новую директорию
        idx = fs_find_free_slot();
        if (idx < 0) return -1;
        
        // Копируем имя
        i = 0;
        while (dirname[i] && i < FS_FILENAME_LEN - 1) {
            file_table[idx].name[i] = dirname[i];
            i++;
        }
        file_table[idx].name[i] = 0;
        
        // Устанавливаем флаги
        file_table[idx].flags = FS_FLAG_OCCUPIED | FS_FLAG_DIRECTORY;
        file_table[idx].parent_dir = current_parent;
        file_table[idx].start_sector = 0; // Директории не занимают секторы данных
        file_table[idx].size_bytes = 0;
        
        boot_sector.file_count++;
        current_parent = idx; // Переходим в созданную директорию
        
        // Сохраняем таблицу после каждого создания
        if (fs_save_file_table() != 0) {
            return -1;
        }
    }
    
    return 0;
}

