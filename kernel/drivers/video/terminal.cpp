#include "terminal.h"
#include "driver_manager.h"
#include "serial_log.h"
#include <stddef.h>
#include <stdint.h>

// VGA буфер (прямой доступ к байтам)
static volatile uint8_t* const VGA_MEMORY_PTR = (volatile uint8_t*) VGA_MEMORY;
static size_t terminal_row = 0;
static size_t terminal_column = 0;
static uint8_t terminal_color = 0x07; // Светло-серый на черном

// Порты VGA курсора
static inline void outb(uint16_t port, uint8_t val) { asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t ret; asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port)); return ret; }

// Текущее разрешение экрана
static size_t current_vga_width = VGA_WIDTH;
static size_t current_vga_height = VGA_HEIGHT;

// Scrollback: history of lines that scrolled off + live screen rows
#define SCROLLBACK_SIZE 500
static uint16_t history_lines[SCROLLBACK_SIZE * 80];
static size_t history_count = 0;
static size_t view_first_line = 0;
static bool scrollback_mode = false;

static struct {
    size_t row;
    size_t col;
    size_t view_first;
    bool scrollback;
} editor_saved;
static bool editor_mode = false;

// Обновление курсора
static void terminal_update_cursor() {
    if (terminal_row >= current_vga_height) terminal_row = current_vga_height > 0 ? current_vga_height - 1 : 0;
    if (terminal_column >= current_vga_width) terminal_column = current_vga_width > 0 ? current_vga_width - 1 : 0;
    uint16_t phys_col = terminal_column < 80 ? terminal_column : 79;
    uint16_t pos = (uint16_t)(terminal_row * 80 + phys_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void history_push_vga_row(size_t row) {
    const size_t vga_width = 80;
    if (row >= current_vga_height) return;
    if (history_count >= SCROLLBACK_SIZE) {
        for (size_t i = 0; i < (SCROLLBACK_SIZE - 1) * vga_width; i++) {
            history_lines[i] = history_lines[i + vga_width];
        }
        history_count = SCROLLBACK_SIZE - 1;
    }
    size_t dst = history_count * vga_width;
    for (size_t col = 0; col < vga_width; col++) {
        size_t vga_idx = row * vga_width + col;
        if (vga_idx < 4800) {
            history_lines[dst + col] =
                (uint16_t)VGA_MEMORY_PTR[vga_idx * 2] |
                ((uint16_t)VGA_MEMORY_PTR[vga_idx * 2 + 1] << 8);
        } else {
            history_lines[dst + col] = (uint16_t)' ' | ((uint16_t)terminal_color << 8);
        }
    }
    history_count++;
}

static void terminal_render_view(size_t first_line) {
    const size_t vga_width = 80;
    size_t total_lines = history_count + current_vga_height;
    if (first_line + current_vga_height > total_lines) {
        if (total_lines > current_vga_height) {
            first_line = total_lines - current_vga_height;
        } else {
            first_line = 0;
        }
    }
    view_first_line = first_line;
    scrollback_mode = (first_line < history_count);

    for (size_t row = 0; row < current_vga_height; row++) {
        size_t line_idx = first_line + row;
        for (size_t col = 0; col < vga_width; col++) {
            uint16_t entry;
            if (line_idx < history_count) {
                entry = history_lines[line_idx * vga_width + col];
            } else {
                size_t vga_row = line_idx - history_count;
                size_t vga_idx = vga_row * vga_width + col;
                if (vga_idx < 4800) {
                    entry = (uint16_t)VGA_MEMORY_PTR[vga_idx * 2] |
                            ((uint16_t)VGA_MEMORY_PTR[vga_idx * 2 + 1] << 8);
                } else {
                    entry = (uint16_t)' ' | ((uint16_t)terminal_color << 8);
                }
            }
            size_t vga_idx = row * vga_width + col;
            if (vga_idx < 4800) {
                VGA_MEMORY_PTR[vga_idx * 2] = (uint8_t)(entry & 0xFF);
                VGA_MEMORY_PTR[vga_idx * 2 + 1] = (uint8_t)((entry >> 8) & 0xFF);
            }
        }
    }
    terminal_row = current_vga_height - 1;
    terminal_column = 0;
    terminal_update_cursor();
}

static void terminal_exit_scrollback() {
    if (!scrollback_mode) return;
    scrollback_mode = false;
    view_first_line = history_count;
    terminal_render_view(history_count);
}

// Прокрутка экрана вверх на указанное количество строк
static void terminal_scroll_lines(size_t lines) {
    if (lines == 0 || current_vga_height == 0) return;
    const size_t vga_width = 80;
    for (size_t n = 0; n < lines; n++) {
        history_push_vga_row(0);
        if (current_vga_height > 1) {
            for (size_t row = 0; row < current_vga_height - 1; row++) {
                for (size_t col = 0; col < vga_width; col++) {
                    size_t src_idx = (row + 1) * vga_width + col;
                    size_t dst_idx = row * vga_width + col;
                    if (src_idx < 4800 && dst_idx < 4800) {
                        VGA_MEMORY_PTR[dst_idx * 2] = VGA_MEMORY_PTR[src_idx * 2];
                        VGA_MEMORY_PTR[dst_idx * 2 + 1] = VGA_MEMORY_PTR[src_idx * 2 + 1];
                    }
                }
            }
        }
        size_t last_row = current_vga_height - 1;
        for (size_t col = 0; col < vga_width; col++) {
            size_t idx = last_row * vga_width + col;
            if (idx < 4800) {
                VGA_MEMORY_PTR[idx * 2] = 0x20;
                VGA_MEMORY_PTR[idx * 2 + 1] = terminal_color;
            }
        }
    }
    if (scrollback_mode) {
        terminal_render_view(view_first_line);
    }
}

void terminal_scroll_up() {
    if (history_count == 0) return;
    size_t first = scrollback_mode ? view_first_line : history_count;
    if (first == 0) return;
    terminal_render_view(first - 1);
}

void terminal_scroll_down() {
    if (!scrollback_mode) return;
    if (view_first_line >= history_count) {
        terminal_exit_scrollback();
        return;
    }
    terminal_render_view(view_first_line + 1);
}

void terminal_scroll_page_up() {
    size_t first = scrollback_mode ? view_first_line : history_count;
    if (first == 0) return;
    size_t step = current_vga_height > 1 ? current_vga_height - 1 : 1;
    if (first > step) terminal_render_view(first - step);
    else terminal_render_view(0);
}

void terminal_scroll_page_down() {
    if (!scrollback_mode) return;
    size_t step = current_vga_height > 1 ? current_vga_height - 1 : 1;
    if (view_first_line + step < history_count) {
        terminal_render_view(view_first_line + step);
    } else {
        terminal_exit_scrollback();
    }
}

bool terminal_in_scrollback() {
    return scrollback_mode;
}

// Установка VGA режима
void terminal_set_mode(size_t width, size_t height) {
    width = 80;
    height = 25;
    terminal_row = 0;
    terminal_column = 0;
    current_vga_width = width;
    current_vga_height = height;
    for (size_t i = 0; i < 80 * 60 * 2; i += 2) {
        VGA_MEMORY_PTR[i] = 0x20;
        VGA_MEMORY_PTR[i + 1] = terminal_color;
    }
    terminal_row = 0;
    terminal_column = 0;
    terminal_update_cursor();
}

// Создание цвета VGA
static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | bg << 4;
}

// Запись символа в VGA
static inline void vga_write_char(size_t index, char c, uint8_t color) {
    VGA_MEMORY_PTR[index * 2] = (uint8_t)c;
    VGA_MEMORY_PTR[index * 2 + 1] = color;
}

// Инициализация терминала
// Функции-обертки для driver_manager
static int terminal_driver_read(void* device_data, void* buffer, size_t size, uint32_t offset) {
    (void)device_data;
    // Читаем содержимое экрана (offset = row * width + col)
    const size_t vga_width = 80;
    size_t row = offset / vga_width;
    size_t col = offset % vga_width;
    
    char* buf = (char*)buffer;
    size_t read = 0;
    
    // Читаем символы из VGA памяти
    for (size_t i = 0; i < size && row < current_vga_height && col < vga_width; i++) {
        size_t vga_idx = row * vga_width + col;
        if (vga_idx < 4800) {
            buf[read++] = (char)VGA_MEMORY_PTR[vga_idx * 2];
        }
        col++;
        if (col >= vga_width) {
            col = 0;
            row++;
        }
    }
    
    return read;
}

static int terminal_driver_write(void* device_data, const void* buffer, size_t size, uint32_t offset) {
    (void)device_data;
    (void)offset;  // offset не используется, пишем в текущую позицию курсора
    
    const char* buf = (const char*)buffer;
    for (size_t i = 0; i < size; i++) {
        terminal_putchar(buf[i]);
    }
    return size;
}

static int terminal_driver_ioctl(void* device_data, uint32_t cmd, void* arg) {
    (void)device_data;
    // IOCTL команды для терминала
    // cmd = 0: установить цвет (arg = uint8_t* color)
    // cmd = 1: установить позицию курсора (arg = struct {size_t row, size_t col}*)
    // cmd = 2: получить позицию курсора (arg = struct {size_t row, size_t col}*)
    // cmd = 3: получить размер экрана (arg = struct {size_t width, size_t height}*)
    if (cmd == 0 && arg) {
        uint8_t* color = (uint8_t*)arg;
        terminal_setcolor(*color);
        return 0;
    } else if (cmd == 1 && arg) {
        struct {
            size_t row;
            size_t col;
        }* pos = (typeof(pos))arg;
        terminal_set_cursor(pos->row, pos->col);
        return 0;
    } else if (cmd == 2 && arg) {
        struct {
            size_t row;
            size_t col;
        }* pos = (typeof(pos))arg;
        pos->row = terminal_get_row();
        pos->col = terminal_get_column();
        return 0;
    } else if (cmd == 3 && arg) {
        struct {
            size_t width;
            size_t height;
        }* size = (typeof(size))arg;
        size->width = current_vga_width;
        size->height = current_vga_height;
        return 0;
    }
    return -1;
}

void terminal_initialize() {
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    history_count = 0;
    view_first_line = 0;
    scrollback_mode = false;
    const size_t vga_width = 80;
    for (size_t i = 0; i < vga_width * current_vga_height * 2; i += 2) {
        VGA_MEMORY_PTR[i] = 0x20;
        VGA_MEMORY_PTR[i + 1] = terminal_color;
    }
    terminal_row = 0;
    terminal_column = 0;
    terminal_update_cursor();
    
    // Регистрируем драйвер в менеджере драйверов
    struct driver terminal_driver;
    terminal_driver.name[0] = 't';
    terminal_driver.name[1] = 't';
    terminal_driver.name[2] = 'y';
    terminal_driver.name[3] = '0';
    terminal_driver.name[4] = 0;
    terminal_driver.type = DRIVER_VIDEO;
    terminal_driver.device_id = 0;  // Будет присвоен автоматически
    terminal_driver.device_data = 0;  // Не нужны специфичные данные
    terminal_driver.initialized = true;
    terminal_driver.active = true;
    
    terminal_driver.ops.init = 0;  // Уже инициализирован
    terminal_driver.ops.read = terminal_driver_read;
    terminal_driver.ops.write = terminal_driver_write;
    terminal_driver.ops.ioctl = terminal_driver_ioctl;
    terminal_driver.ops.cleanup = 0;
    
    driver_register(&terminal_driver);
}

// Установка цвета
void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

uint8_t terminal_getcolor() {
    return terminal_color;
}

// Вывод символа
void terminal_putchar(char c) {
    const size_t vga_width = 80;
    if (scrollback_mode) {
        terminal_exit_scrollback();
    }
    if (c == '\n') {
        terminal_column = 0;
        terminal_row++;
        if (terminal_row >= current_vga_height) {
            size_t lines_to_scroll = terminal_row - current_vga_height + 1;
            terminal_scroll_lines(lines_to_scroll);
            terminal_row = current_vga_height - 1;
        }
    } else if (c == '\b') {
        if (terminal_column > 0) {
            terminal_column--;
        } else if (terminal_row > 0) {
            terminal_row--;
            terminal_column = current_vga_width - 1;
        }
        if (terminal_row < current_vga_height) {
            const size_t index = terminal_row * vga_width + (terminal_column < vga_width ? terminal_column : vga_width - 1);
            if (index < 4800) {
                vga_write_char(index, ' ', terminal_color);
            }
        }
    } else {
        if (terminal_row < current_vga_height && terminal_column < current_vga_width) {
            const size_t index = terminal_row * vga_width + terminal_column;
            if (index < 4800) {
                vga_write_char(index, c, terminal_color);
            }
        }
        terminal_column++;
        if (terminal_column >= current_vga_width) {
            terminal_column = 0;
            terminal_row++;
        }
        if (terminal_row >= current_vga_height) {
            size_t lines_to_scroll = terminal_row - current_vga_height + 1;
            terminal_scroll_lines(lines_to_scroll);
            terminal_row = current_vga_height - 1;
            if (terminal_column >= current_vga_width) {
                terminal_column = 0;
            }
        }
    }
    terminal_update_cursor();
    log_mirror_char(c);
}

// Вывод строки
void terminal_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

// Вывод строки (C строка)
void terminal_writestring(const char* data) {
    while (*data)
        terminal_putchar(*data++);
}

// Установка курсора
void terminal_set_cursor(size_t row, size_t col) {
    terminal_row = row;
    terminal_column = col;
    terminal_update_cursor();
}

// Получить/установить позицию курсора
size_t terminal_get_row() { return terminal_row; }
size_t terminal_get_column() { return terminal_column; }
size_t terminal_get_height() { return current_vga_height; }
void terminal_set_row(size_t row) { terminal_row = row; terminal_update_cursor(); }
void terminal_set_column(size_t col) { terminal_column = col; terminal_update_cursor(); }

void terminal_editor_begin() {
    editor_saved.row = terminal_row;
    editor_saved.col = terminal_column;
    editor_saved.view_first = view_first_line;
    editor_saved.scrollback = scrollback_mode;
    editor_mode = true;
    scrollback_mode = false;

    const size_t vga_width = 80;
    for (size_t row = 0; row < current_vga_height; row++) {
        for (size_t col = 0; col < vga_width; col++) {
            size_t idx = row * vga_width + col;
            if (idx < 4800) {
                VGA_MEMORY_PTR[idx * 2] = 0x20;
                VGA_MEMORY_PTR[idx * 2 + 1] = terminal_color;
            }
        }
    }
    terminal_row = 0;
    terminal_column = 0;
    terminal_update_cursor();
}

void terminal_editor_end() {
    editor_mode = false;
    scrollback_mode = editor_saved.scrollback;
    view_first_line = editor_saved.view_first;
    terminal_render_view(editor_saved.scrollback ? view_first_line : history_count);
    terminal_row = editor_saved.row;
    terminal_column = editor_saved.col;
    if (terminal_row >= current_vga_height) {
        terminal_row = current_vga_height > 0 ? current_vga_height - 1 : 0;
    }
    if (terminal_column >= current_vga_width) {
        terminal_column = current_vga_width > 0 ? current_vga_width - 1 : 0;
    }
    terminal_update_cursor();
}

void terminal_editor_putat(size_t row, size_t col, char c, uint8_t color) {
    if (row >= current_vga_height || col >= 80) return;
    size_t idx = row * 80 + col;
    if (idx < 4800) {
        VGA_MEMORY_PTR[idx * 2] = (uint8_t)c;
        VGA_MEMORY_PTR[idx * 2 + 1] = color;
    }
}

void terminal_editor_print_at(size_t row, size_t col, const char* s, uint8_t color) {
    if (!s) return;
    while (*s && col < 80) {
        terminal_editor_putat(row, col++, *s++, color);
    }
}

void terminal_editor_clear_row(size_t row, uint8_t color) {
    for (size_t col = 0; col < 80; col++) {
        terminal_editor_putat(row, col, ' ', color);
    }
}

void terminal_editor_show_cursor(size_t row, size_t col) {
    terminal_row = row;
    terminal_column = col;
    terminal_update_cursor();
}

