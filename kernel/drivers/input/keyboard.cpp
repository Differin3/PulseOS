#include "keyboard.h"
#include "kernel.h"
#include "driver_manager.h"
#include "drivers/pic/pic.h"
#include <stdint.h>
#include <stddef.h>

// Буфер для символов
#define KEYBOARD_BUFFER_SIZE 256
static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static uint32_t keyboard_buffer_head = 0;
static uint32_t keyboard_buffer_tail = 0;
static volatile uint32_t keyboard_irq_hits = 0;
static volatile uint32_t keyboard_poll_hits = 0;

// Таблица скан-кодов (US QWERTY) - глобальная
const char SCANCODE_TABLE[SCANCODE_TABLE_SIZE] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

const char SCANCODE_SHIFT_TABLE[SCANCODE_TABLE_SIZE] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

static bool kb_shift = false;
static bool kb_ctrl = false;

void keyboard_modifiers_update(uint8_t scancode, bool extended) {
    (void)extended;
    bool release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    if (code == 0x2A || code == 0x36) {
        kb_shift = !release;
        return;
    }
    if (code == 0x1D) {
        kb_ctrl = !release;
    }
}

char keyboard_scancode_char(uint8_t scancode) {
    if (scancode >= SCANCODE_TABLE_SIZE) return 0;
    char c = SCANCODE_TABLE[scancode];
    if (c == 0) return 0;
    if (kb_shift) {
        char s = SCANCODE_SHIFT_TABLE[scancode];
        if (s != 0) return s;
    }
    return c;
}

bool keyboard_shift_down(void) {
    return kb_shift;
}

bool keyboard_ctrl_down(void) {
    return kb_ctrl;
}

// Чтение из порта
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Запись в порт
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Ждать пока входной буфер контроллера свободен (бит1=0)
static void ps2_wait_input_clear() {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(0x64);
        if ((st & 0x02) == 0) break;
    }
}

// Минимальная инициализация PS/2 клавиатуры
static void ps2_enable_keyboard() {
    // Включить первый порт
    ps2_wait_input_clear();
    outb(0x64, 0xAE);
    // Включить сканирование
    ps2_wait_input_clear();
    outb(KEYBOARD_DATA_PORT, 0xF4);
}

// Обработчик прерывания клавиатуры
extern "C" void keyboard_handler_main() {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    keyboard_irq_hits++;

    keyboard_modifiers_update(scancode, false);

    if (scancode & 0x80) {
        pic_eoi(1);
        return;
    }
    if (scancode == 0x2A || scancode == 0x36 || scancode == 0x1D) {
        pic_eoi(1);
        return;
    }
    if (scancode >= SCANCODE_TABLE_SIZE) {
        pic_eoi(1);
        return;
    }

    char c = keyboard_scancode_char(scancode);
    if (c != 0) {
        // Добавляем в буфер
        uint32_t next = (keyboard_buffer_tail + 1) % KEYBOARD_BUFFER_SIZE;
        if (next != keyboard_buffer_head) {
            keyboard_buffer[keyboard_buffer_tail] = c;
            keyboard_buffer_tail = next;
        }
    }
}

// Функции-обертки для driver_manager
static int keyboard_driver_read(void* device_data, void* buffer, size_t size, uint32_t offset) {
    (void)device_data;
    (void)offset;  // offset не используется для клавиатуры
    
    char* buf = (char*)buffer;
    size_t read = 0;
    
    // Читаем символы из буфера клавиатуры
    while (read < size && keyboard_buffer_head != keyboard_buffer_tail) {
        buf[read++] = keyboard_getchar();
    }
    
    return read;
}

static int keyboard_driver_write(void* device_data, const void* buffer, size_t size, uint32_t offset) {
    (void)device_data;
    (void)buffer;
    (void)size;
    (void)offset;
    // Клавиатура не поддерживает запись
    return -1;
}

static int keyboard_driver_ioctl(void* device_data, uint32_t cmd, void* arg) {
    (void)device_data;
    // IOCTL команды для клавиатуры
    // cmd = 0: получить количество символов в буфере
    // cmd = 1: получить статистику (irq_count, poll_count)
    if (cmd == 0 && arg) {
        uint32_t* count = (uint32_t*)arg;
        uint32_t available = (keyboard_buffer_tail >= keyboard_buffer_head) ?
            (keyboard_buffer_tail - keyboard_buffer_head) :
            (KEYBOARD_BUFFER_SIZE - keyboard_buffer_head + keyboard_buffer_tail);
        *count = available;
        return 0;
    } else if (cmd == 1 && arg) {
        struct {
            uint32_t irq_count;
            uint32_t poll_count;
        }* stats = (typeof(stats))arg;
        stats->irq_count = keyboard_irq_count();
        stats->poll_count = keyboard_poll_count();
        return 0;
    }
    return -1;
}

// Инициализация клавиатуры
void keyboard_init() {
    ps2_enable_keyboard();
    keyboard_buffer_head = 0;
    keyboard_buffer_tail = 0;
    pic_unmask(1);
    
    // Регистрируем драйвер в менеджере драйверов
    struct driver keyboard_driver;
    keyboard_driver.name[0] = 'k';
    keyboard_driver.name[1] = 'b';
    keyboard_driver.name[2] = 'd';
    keyboard_driver.name[3] = '0';
    keyboard_driver.name[4] = 0;
    keyboard_driver.type = DRIVER_INPUT;
    keyboard_driver.device_id = 0;  // Будет присвоен автоматически
    keyboard_driver.device_data = 0;  // Не нужны специфичные данные
    keyboard_driver.initialized = true;
    keyboard_driver.active = true;
    
    keyboard_driver.ops.init = 0;  // Уже инициализирован
    keyboard_driver.ops.read = keyboard_driver_read;
    keyboard_driver.ops.write = keyboard_driver_write;
    keyboard_driver.ops.ioctl = keyboard_driver_ioctl;
    keyboard_driver.ops.cleanup = 0;
    
    driver_register(&keyboard_driver);
}

// Получить символ из буфера
char keyboard_getchar() {
    if (keyboard_buffer_head == keyboard_buffer_tail) {
        return 0; // Буфер пуст
    }
    char c = keyboard_buffer[keyboard_buffer_head];
    keyboard_buffer_head = (keyboard_buffer_head + 1) % KEYBOARD_BUFFER_SIZE;
    return c;
}

uint32_t keyboard_irq_count() {
    return keyboard_irq_hits;
}

uint32_t keyboard_poll_count() {
    return keyboard_poll_hits;
}

void keyboard_poll_hit() {
    keyboard_poll_hits++;
}

