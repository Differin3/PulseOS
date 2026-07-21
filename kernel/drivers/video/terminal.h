#ifndef TERMINAL_H
#define TERMINAL_H

#include "kernel.h"
#include <stddef.h>
#include <stdint.h>

// Инициализация терминала
void terminal_initialize();
void terminal_setcolor(uint8_t color);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void terminal_writestring(const char* data);
void terminal_set_cursor(size_t row, size_t col);

// Прокрутка
void terminal_scroll_up();
void terminal_scroll_down();
void terminal_scroll_page_up();
void terminal_scroll_page_down();
bool terminal_in_scrollback();

// Получить текущую позицию курсора
size_t terminal_get_row();
size_t terminal_get_column();
size_t terminal_get_height();
void terminal_set_row(size_t row);
void terminal_set_column(size_t col);

// Установка режима VGA
void terminal_set_mode(size_t width, size_t height);

// Получить текущий цвет
uint8_t terminal_getcolor();

// Полноэкранный редактор (nano): прямой вывод в VGA без scrollback
void terminal_editor_begin();
void terminal_editor_end();
void terminal_editor_putat(size_t row, size_t col, char c, uint8_t color);
void terminal_editor_print_at(size_t row, size_t col, const char* s, uint8_t color);
void terminal_editor_clear_row(size_t row, uint8_t color);
void terminal_editor_show_cursor(size_t row, size_t col);

#endif

