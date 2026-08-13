#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>

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
#define SYS_EXIT        15
#define SYS_YIELD       16
#define SYS_RING3_DONE  17
#define SYS_LSEEK       18
#define SYS_STAT        19
#define SYS_FSTAT       20
#define SYS_DUP         21
#define SYS_FSYNC       22
#define SYS_LINK        23
#define SYS_UNLINK      24
#define SYS_CHMOD       25
#define SYS_SYNC        26
#define SYS_OPENAT      27
#define SYS_GETDENTS    28
#define SYS_MMAP_RO     29

struct syscall_args {
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
    uint32_t arg4;
};

extern "C" int syscall_handler(struct syscall_args* args);
void syscall_init();

#endif
