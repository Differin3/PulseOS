#ifndef SERIAL_LOG_H
#define SERIAL_LOG_H

#include <stdint.h>
#include <stddef.h>

#define LOG_OFF  0
#define LOG_ERR  1
#define LOG_INFO 2
#define LOG_DBG  3

void serial_init(void);
void log_set_level(int level);
int log_get_level(void);

void log_msg(int level, const char* tag, const char* message);
void log_u32(int level, const char* tag, const char* message,
             uint32_t a, uint32_t b, uint32_t c);
void log_fmt3(int level, const char* tag, const char* message,
              const char* k1, uint32_t v1,
              const char* k2, uint32_t v2,
              const char* k3, uint32_t v3);
void log_ip(int level, const char* tag, const char* message, uint32_t ip);

// Зеркало VGA-терминала в COM1 (logs/qemu-serial.log)
void log_mirror_set(bool enabled);
bool log_mirror_get(void);
void log_mirror_set_input(bool is_user_typing);
void log_mirror_char(char c);
void log_shell_cmd(const char* cwd, const char* cmd, size_t len);
void log_driver_event(const char* name, const char* event);

// Двусторонний COM1: ввод с хоста (QEMU -serial tcp:...)
char serial_poll_char(void);

#endif
