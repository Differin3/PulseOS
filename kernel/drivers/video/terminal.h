#ifndef TERMINAL_H
#define TERMINAL_H

#include "kernel.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void terminal_initialize();
/* After paging_init: enable Multiboot2 framebuffer if present. */
void terminal_init_graphics(uint32_t multiboot_info);

void terminal_setcolor(uint8_t color);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void terminal_writestring(const char* data);
void terminal_set_cursor(size_t row, size_t col);

void terminal_scroll_up();
void terminal_scroll_down();
void terminal_scroll_page_up();
void terminal_scroll_page_down();
bool terminal_in_scrollback();

size_t terminal_get_row();
size_t terminal_get_column();
size_t terminal_get_height();
size_t terminal_get_width();
size_t terminal_content_height(); /* rows in content band (excludes header/status) */
size_t terminal_content_origin(); /* first content row (1 when header on) */
void terminal_set_row(size_t row);
void terminal_set_column(size_t col);

void terminal_set_mode(size_t width, size_t height);
uint8_t terminal_getcolor();

void terminal_clear_viewport(void);
void terminal_put_at(size_t row, size_t col, char c, uint8_t color);
void terminal_write_at(size_t row, size_t col, const char* s, uint8_t color);

/* Status bar (bottom). set() fills left zone for autotest compat. */
void terminal_status_set(const char* text);
void terminal_status_set_zones(const char* left, const char* mid, const char* right);
void terminal_status_redraw(void);

/* Header bar (top). */
void terminal_header_set(const char* left, const char* right);
void terminal_header_redraw(void);

bool terminal_using_framebuffer(void);
uint32_t terminal_fb_width(void);
uint32_t terminal_fb_height(void);
uint32_t terminal_fb_bpp(void);

/* Read cell for autotest: returns char in low 8, attr in high 8; -1 on fail */
int terminal_read_cell(size_t row, size_t col);

void terminal_editor_begin();
void terminal_editor_end();
void terminal_editor_putat(size_t row, size_t col, char c, uint8_t color);
void terminal_editor_print_at(size_t row, size_t col, const char* s, uint8_t color);
void terminal_editor_clear_row(size_t row, uint8_t color);
void terminal_editor_show_cursor(size_t row, size_t col);

#endif
