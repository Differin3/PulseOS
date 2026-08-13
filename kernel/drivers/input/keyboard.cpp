#include "keyboard.h"
#include "kernel.h"
#include "driver_manager.h"
#include "drivers/pic/pic.h"
#include <stdint.h>
#include <stddef.h>

#define KB_BUF_SIZE 256

static uint8_t kb_buf[KB_BUF_SIZE];
static volatile uint32_t kb_head = 0;
static volatile uint32_t kb_tail = 0;
static volatile uint32_t kb_irq_hits = 0;
static volatile uint32_t kb_drops = 0;

static volatile bool kb_e0 = false;
static volatile int kb_e1_skip = 0;

static volatile bool kb_lshift = false;
static volatile bool kb_rshift = false;
static volatile bool kb_lctrl = false;
static volatile bool kb_rctrl = false;
static volatile bool kb_lalt = false;
static volatile bool kb_ralt = false;
static volatile bool kb_caps = false;
static volatile bool kb_num = true;
static volatile bool kb_scroll = false;
static volatile bool kb_leds_dirty = false;
static volatile bool kb_test_mode = false;

/* US QWERTY Set1 (index = make code 0..0x58). 0 = none / modifier / handled elsewhere. */
static const char KB_MAP[0x59] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const char KB_MAP_SHIFT[0x59] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t irq_save(void) {
    uint32_t flags;
    asm volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags) {
    asm volatile("push %0; popf" :: "r"(flags) : "memory");
}

static void kb_push(uint8_t c) {
    if (c == 0) return;
    uint32_t f = irq_save();
    uint32_t next = (kb_tail + 1) % KB_BUF_SIZE;
    if (next == kb_head) {
        kb_drops++;
        irq_restore(f);
        return;
    }
    kb_buf[kb_tail] = c;
    kb_tail = next;
    irq_restore(f);
}

static void ps2_wait_input_clear(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(KEYBOARD_STATUS_PORT) & 0x02) == 0) return;
    }
}

static void ps2_wait_output(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(KEYBOARD_STATUS_PORT) & 0x01) return;
    }
}

static void ps2_write_cmd(uint8_t cmd) {
    ps2_wait_input_clear();
    outb(KEYBOARD_STATUS_PORT, cmd);
}

static void ps2_write_data(uint8_t data) {
    ps2_wait_input_clear();
    outb(KEYBOARD_DATA_PORT, data);
}

static uint8_t ps2_read_data(void) {
    ps2_wait_output();
    return inb(KEYBOARD_DATA_PORT);
}

static void ps2_flush(void) {
    for (int i = 0; i < 64; i++) {
        if (!(inb(KEYBOARD_STATUS_PORT) & 0x01)) break;
        (void)inb(KEYBOARD_DATA_PORT);
    }
}

static void kb_apply_leds(void) {
    uint8_t leds = 0;
    if (kb_scroll) leds |= 0x01;
    if (kb_num) leds |= 0x02;
    if (kb_caps) leds |= 0x04;
    ps2_write_data(0xED);
    ps2_wait_output();
    (void)inb(KEYBOARD_DATA_PORT); /* ACK */
    ps2_write_data(leds);
    ps2_wait_output();
    (void)inb(KEYBOARD_DATA_PORT);
    kb_leds_dirty = false;
}

static void kb_leds_maybe_flush(void) {
    if (!kb_leds_dirty) return;
    if (kb_test_mode) {
        kb_leds_dirty = false;
        return;
    }
    kb_apply_leds();
}

static bool kb_shift(void) {
    return kb_lshift || kb_rshift;
}

static bool kb_ctrl(void) {
    return kb_lctrl || kb_rctrl;
}

static char kb_letter_case(char c) {
    if (c >= 'a' && c <= 'z') {
        bool upper = kb_shift() ^ kb_caps;
        if (upper) return (char)(c - 32);
        return c;
    }
    if (c >= 'A' && c <= 'Z') {
        bool upper = kb_shift() ^ kb_caps;
        if (!upper) return (char)(c + 32);
        return c;
    }
    return c;
}

static uint8_t kb_numpad_key(uint8_t code, bool extended) {
    if (extended) return 0;
    if (!kb_num) {
        switch (code) {
            case 0x47: return KEY_HOME;
            case 0x48: return KEY_UP;
            case 0x49: return KEY_PGUP;
            case 0x4B: return KEY_LEFT;
            case 0x4D: return KEY_RIGHT;
            case 0x4F: return KEY_END;
            case 0x50: return KEY_DOWN;
            case 0x51: return KEY_PGDN;
            case 0x52: return KEY_INSERT;
            case 0x53: return KEY_DELETE;
            case 0x4C: return 0;
            default: break;
        }
    }
    switch (code) {
        case 0x47: return '7';
        case 0x48: return '8';
        case 0x49: return '9';
        case 0x4A: return '-';
        case 0x4B: return '4';
        case 0x4C: return '5';
        case 0x4D: return '6';
        case 0x4E: return '+';
        case 0x4F: return '1';
        case 0x50: return '2';
        case 0x51: return '3';
        case 0x52: return '0';
        case 0x53: return '.';
        case 0x37: return '*';
        default: return 0;
    }
}

static uint8_t kb_extended_special(uint8_t code) {
    switch (code) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x52: return KEY_INSERT;
        case 0x53: return KEY_DELETE;
        case 0x1C: return '\n'; /* KP Enter */
        case 0x35: return '/';  /* KP / */
        default: return 0;
    }
}

static uint8_t kb_f_key(uint8_t code) {
    if (code >= 0x3B && code <= 0x44) return (uint8_t)(KEY_F1 + (code - 0x3B));
    if (code == 0x57) return KEY_F11;
    if (code == 0x58) return KEY_F12;
    return 0;
}

static void kb_handle_byte(uint8_t scancode) {
    if (scancode == 0xE0) {
        kb_e0 = true;
        return;
    }
    if (scancode == 0xE1) {
        /* Pause: E1 1D 45 E1 9D C5 — skip remaining bytes */
        kb_e1_skip = 5;
        return;
    }
    if (kb_e1_skip > 0) {
        kb_e1_skip--;
        return;
    }

    bool extended = kb_e0;
    kb_e0 = false;

    bool release = (scancode & 0x80) != 0;
    uint8_t code = (uint8_t)(scancode & 0x7F);

    /* Modifiers */
    if (code == 0x2A) { kb_lshift = !release; return; }
    if (code == 0x36) { kb_rshift = !release; return; }
    if (code == 0x1D) {
        if (extended) kb_rctrl = !release;
        else kb_lctrl = !release;
        return;
    }
    if (code == 0x38) {
        if (extended) kb_ralt = !release;
        else kb_lalt = !release;
        return;
    }

    if (release) return;

    if (!extended && code == 0x3A) {
        kb_caps = !kb_caps;
        kb_leds_dirty = true;
        return;
    }
    if (!extended && code == 0x45) {
        kb_num = !kb_num;
        kb_leds_dirty = true;
        return;
    }
    if (!extended && code == 0x46) {
        kb_scroll = !kb_scroll;
        kb_leds_dirty = true;
        return;
    }

    if (extended) {
        uint8_t sp = kb_extended_special(code);
        if (sp) kb_push(sp);
        return;
    }

    uint8_t fk = kb_f_key(code);
    if (fk) {
        kb_push(fk);
        return;
    }

    if (code >= 0x47 && code <= 0x53) {
        uint8_t np = kb_numpad_key(code, false);
        if (np) kb_push(np);
        return;
    }
    if (code == 0x37) {
        kb_push('*');
        return;
    }

    if (code >= 0x59) return;
    char base = KB_MAP[code];
    if (base == 0) return;

    char out = base;
    if (base >= 'a' && base <= 'z') {
        out = kb_letter_case(base);
    } else if (kb_shift()) {
        char s = KB_MAP_SHIFT[code];
        if (s != 0) out = s;
    }

    if (kb_ctrl() && out >= 'a' && out <= 'z') {
        kb_push((uint8_t)(out - 'a' + 1));
        return;
    }
    if (kb_ctrl() && out >= 'A' && out <= 'Z') {
        kb_push((uint8_t)(out - 'A' + 1));
        return;
    }

    kb_push((uint8_t)out);
}

void keyboard_inject_scancode(uint8_t scancode) {
    kb_handle_byte(scancode);
}

void keyboard_test_end(void) {
    uint32_t f = irq_save();
    kb_test_mode = false;
    kb_leds_dirty = false;
    irq_restore(f);
}

void keyboard_test_reset(void) {
    uint32_t f = irq_save();
    kb_test_mode = true;
    kb_head = 0;
    kb_tail = 0;
    kb_drops = 0;
    kb_e0 = false;
    kb_e1_skip = 0;
    kb_lshift = kb_rshift = false;
    kb_lctrl = kb_rctrl = false;
    kb_lalt = kb_ralt = false;
    kb_caps = false;
    kb_num = true;
    kb_scroll = false;
    kb_leds_dirty = false;
    irq_restore(f);
}

extern "C" void keyboard_handler_main(void) {
    kb_irq_hits++;
    /* Drain all pending bytes — controller overwrites 0x60 if we lag. */
    int guard = 32;
    while ((inb(KEYBOARD_STATUS_PORT) & 0x01) && guard-- > 0) {
        uint8_t sc = inb(KEYBOARD_DATA_PORT);
        kb_handle_byte(sc);
    }
    /* EOI is issued by keyboard_handler in interrupts.asm */
}

static int keyboard_driver_read(void* device_data, void* buffer, size_t size, uint32_t offset) {
    (void)device_data;
    (void)offset;
    char* buf = (char*)buffer;
    size_t read = 0;
    while (read < size) {
        char c = keyboard_getchar();
        if (c == 0) break;
        buf[read++] = c;
    }
    return (int)read;
}

static int keyboard_driver_write(void* device_data, const void* buffer, size_t size, uint32_t offset) {
    (void)device_data;
    (void)buffer;
    (void)size;
    (void)offset;
    return -1;
}

static int keyboard_driver_ioctl(void* device_data, uint32_t cmd, void* arg) {
    (void)device_data;
    if (cmd == 0 && arg) {
        *(uint32_t*)arg = keyboard_available();
        return 0;
    }
    if (cmd == 1 && arg) {
        struct {
            uint32_t irq_count;
            uint32_t drop_count;
        }* stats = (typeof(stats))arg;
        stats->irq_count = keyboard_irq_count();
        stats->drop_count = keyboard_drop_count();
        return 0;
    }
    return -1;
}

void keyboard_init(void) {
    /* Disable devices, flush, program controller, enable IRQ1 scanning. */
    ps2_write_cmd(0xAD);
    ps2_write_cmd(0xA7);
    ps2_flush();

    ps2_write_cmd(0x20);
    uint8_t cfg = ps2_read_data();
    cfg |= 0x01;   /* IRQ1 */
    cfg &= (uint8_t)~0x10; /* enable keyboard clock */
    cfg &= (uint8_t)~0x20; /* clear mouse clock disable bit side-effect ok */
    cfg |= 0x40;   /* translation */
    cfg &= (uint8_t)~0x02; /* no mouse IRQ */
    ps2_write_cmd(0x60);
    ps2_write_data(cfg);

    ps2_write_cmd(0xAE);
    ps2_flush();
    ps2_write_data(0xF4);
    ps2_wait_output();
    (void)inb(KEYBOARD_DATA_PORT); /* ACK */

    keyboard_test_reset();
    kb_test_mode = false;
    kb_num = true;
    kb_apply_leds();

    pic_unmask(1);

    struct driver keyboard_driver;
    keyboard_driver.name[0] = 'k';
    keyboard_driver.name[1] = 'b';
    keyboard_driver.name[2] = 'd';
    keyboard_driver.name[3] = '0';
    keyboard_driver.name[4] = 0;
    keyboard_driver.type = DRIVER_INPUT;
    keyboard_driver.device_id = 0;
    keyboard_driver.device_data = 0;
    keyboard_driver.initialized = true;
    keyboard_driver.active = true;
    keyboard_driver.ops.init = 0;
    keyboard_driver.ops.read = keyboard_driver_read;
    keyboard_driver.ops.write = keyboard_driver_write;
    keyboard_driver.ops.ioctl = keyboard_driver_ioctl;
    keyboard_driver.ops.cleanup = 0;
    driver_register(&keyboard_driver);
}

char keyboard_getchar(void) {
    kb_leds_maybe_flush();
    uint32_t f = irq_save();
    if (kb_head == kb_tail) {
        irq_restore(f);
        return 0;
    }
    char c = (char)kb_buf[kb_head];
    kb_head = (kb_head + 1) % KB_BUF_SIZE;
    irq_restore(f);
    return c;
}

char keyboard_poll(void) {
    return keyboard_getchar();
}

bool keyboard_shift_down(void) { return kb_shift(); }
bool keyboard_ctrl_down(void) { return kb_ctrl(); }
bool keyboard_alt_down(void) { return kb_lalt || kb_ralt; }
bool keyboard_caps_on(void) { return kb_caps; }
bool keyboard_num_on(void) { return kb_num; }

uint32_t keyboard_irq_count(void) { return kb_irq_hits; }
uint32_t keyboard_drop_count(void) { return kb_drops; }

uint32_t keyboard_available(void) {
    uint32_t f = irq_save();
    uint32_t n = (kb_tail >= kb_head)
        ? (kb_tail - kb_head)
        : (KB_BUF_SIZE - kb_head + kb_tail);
    irq_restore(f);
    return n;
}
