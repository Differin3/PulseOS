#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>

// Номера системных вызовов
#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_IOCTL       4
#define SYS_DEVICE_LIST 5
#define SYS_DEVICE_INFO 6

#define SYS_SOCKET      7
#define SYS_BIND        8
#define SYS_LISTEN      9
#define SYS_ACCEPT      10
#define SYS_CONNECT     11
#define SYS_SEND        12
#define SYS_RECV        13
#define SYS_SOCK_CLOSE  14

// Структура для передачи параметров системного вызова
struct syscall_args {
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
    uint32_t arg4;
};

// Обработчик системных вызовов (вызывается из ассемблера)
extern "C" int syscall_handler(struct syscall_args* args);

// Инициализация системы системных вызовов
void syscall_init();

#endif
