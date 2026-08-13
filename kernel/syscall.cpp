#include "syscall.h"
#include "driver_manager.h"
#include "fs.h"
#include "fs_file.h"
#include "drivers/network/socket.h"
#include "sched/task.h"
#include "mm/paging.h"
#include <stddef.h>

extern "C" void syscall_handler_asm();

extern "C" int syscall_handler(struct syscall_args* args) {
    if (!args) return -1;

    uint32_t syscall_num = args->arg0;

    switch (syscall_num) {
        case SYS_READ: {
            /* Prefer task file fd when arg1 looks like a small fd */
            int fd = (int)args->arg1;
            uint8_t ty = 0;
            int handle = -1;
            if (fd >= 0 && fd < TASK_FD_MAX && task_fd_get(fd, &ty, &handle) == 0 &&
                ty == TASK_FD_FILE) {
                return vfs_fread(fd, (void*)args->arg2, (size_t)args->arg3);
            }
            return driver_read(args->arg1, (void*)args->arg2, (size_t)args->arg3, args->arg4);
        }

        case SYS_WRITE: {
            int fd = (int)args->arg1;
            uint8_t ty = 0;
            int handle = -1;
            if (fd >= 0 && fd < TASK_FD_MAX && task_fd_get(fd, &ty, &handle) == 0 &&
                ty == TASK_FD_FILE) {
                return vfs_fwrite(fd, (const void*)args->arg2, (size_t)args->arg3);
            }
            return driver_write(args->arg1, (const void*)args->arg2, (size_t)args->arg3,
                                args->arg4);
        }

        case SYS_OPEN: {
            const char* filename = (const char*)args->arg1;
            int flags = (int)args->arg2;
            if (!flags) flags = O_RDWR;
            uint16_t mode = (uint16_t)args->arg3;
            if (!mode) mode = FS_MODE_FILE;
            if (!filename) return -1;
            return vfs_open(filename, flags, mode);
        }

        case SYS_CLOSE:
            return vfs_close((int)args->arg1);

        case SYS_LSEEK:
            return vfs_lseek((int)args->arg1, (int32_t)args->arg2, (int)args->arg3);

        case SYS_STAT: {
            struct fs_stat st;
            if (fs_stat((const char*)args->arg1, &st) != 0) return -1;
            if (args->arg2) *(struct fs_stat*)args->arg2 = st;
            return 0;
        }

        case SYS_FSTAT:
            return vfs_fstat((int)args->arg1, (struct fs_stat*)args->arg2);

        case SYS_IOCTL:
            return driver_ioctl(args->arg1, args->arg2, (void*)args->arg3);

        case SYS_DEVICE_LIST:
            return driver_list_by_type((enum driver_type)args->arg1, (struct driver*)args->arg2,
                                      (int)args->arg3);

        case SYS_DEVICE_INFO: {
            struct driver* drv = driver_find_by_id(args->arg1);
            struct driver* info = (struct driver*)args->arg2;
            if (drv && info) {
                *info = *drv;
                return 0;
            }
            return -1;
        }

        case SYS_SOCKET: {
            int s = socket_create((int)args->arg1, (int)args->arg2, (int)args->arg3);
            if (s >= 0) task_fd_alloc(TASK_FD_SOCK, s, 0);
            return s;
        }
        case SYS_BIND:
            return socket_bind((int)args->arg1, (const struct sockaddr_in*)args->arg2);
        case SYS_LISTEN:
            return socket_listen((int)args->arg1, (int)args->arg2);
        case SYS_ACCEPT:
            return socket_accept((int)args->arg1, (int)args->arg2);
        case SYS_CONNECT:
            return socket_connect((int)args->arg1, (const struct sockaddr_in*)args->arg2,
                                  (int)args->arg3);
        case SYS_SEND:
            return socket_send((int)args->arg1, (const void*)args->arg2, (size_t)args->arg3);
        case SYS_RECV:
            return socket_recv((int)args->arg1, (void*)args->arg2, (size_t)args->arg3,
                               (int)args->arg4);
        case SYS_SOCK_CLOSE:
            return socket_close((int)args->arg1);
        case SYS_EXIT:
            task_exit();
            return 0;
        case SYS_YIELD:
            sched_yield();
            return 0;
        case SYS_RING3_DONE:
            paging_ring3_finish();
            return 0;
        default:
            return -1;
    }
}

void syscall_init() {}
