#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64

/* Special keys — outside ASCII / Ctrl range (do not use 1..9). */
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83
#define KEY_PGUP    0x84
#define KEY_PGDN    0x85
#define KEY_HOME    0x86
#define KEY_END     0x87
#define KEY_INSERT  0x88
#define KEY_DELETE  0x89
#define KEY_F1      0x90
#define KEY_F2      0x91
#define KEY_F3      0x92
#define KEY_F4      0x93
#define KEY_F5      0x94
#define KEY_F6      0x95
#define KEY_F7      0x96
#define KEY_F8      0x97
#define KEY_F9      0x98
#define KEY_F10     0x99
#define KEY_F11     0x9A
#define KEY_F12     0x9B

void keyboard_init(void);
#ifdef __cplusplus
extern "C" void keyboard_handler_main(void);
#else
void keyboard_handler_main(void);
#endif

/* Non-blocking: next buffered key (ASCII, Ctrl 1..26, or KEY_*). 0 = empty. */
char keyboard_getchar(void);
char keyboard_poll(void);

bool keyboard_shift_down(void);
bool keyboard_ctrl_down(void);
bool keyboard_alt_down(void);
bool keyboard_caps_on(void);
bool keyboard_num_on(void);

uint32_t keyboard_irq_count(void);
uint32_t keyboard_drop_count(void);
uint32_t keyboard_available(void);

/* Autotest: same decode path as IRQ, no port I/O. */
void keyboard_test_reset(void);
void keyboard_test_end(void);
void keyboard_inject_scancode(uint8_t scancode);

#endif
