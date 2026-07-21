#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

// Порт клавиатуры
#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64

// Таблица скан-кодов (глобальная)
#define SCANCODE_TABLE_SIZE 58
extern const char SCANCODE_TABLE[SCANCODE_TABLE_SIZE];
extern const char SCANCODE_SHIFT_TABLE[SCANCODE_TABLE_SIZE];

// Обновить Shift/Ctrl по scancode (нажатие и отпускание)
void keyboard_modifiers_update(uint8_t scancode, bool extended);

// Символ с учётом Shift (0 = нет символа)
char keyboard_scancode_char(uint8_t scancode);

bool keyboard_shift_down(void);
bool keyboard_ctrl_down(void);

// Инициализация клавиатуры
void keyboard_init();

// Обработчик прерывания клавиатуры
extern "C" void keyboard_handler_main();

// Получить последний нажатый символ (0 если нет)
char keyboard_getchar();
uint32_t keyboard_irq_count();
uint32_t keyboard_poll_count();
void keyboard_poll_hit();

#endif

