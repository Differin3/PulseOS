#include "syscall.h"
#include "driver_manager.h"
#include "fs.h"
#include "drivers/network/socket.h"
#include <stddef.h>

// Внешняя функция из ассемблера
extern "C" void syscall_handler_asm();

// Обработчик системных вызовов
extern "C" int syscall_handler(struct syscall_args* args) {
    if (!args) return -1;
    
    uint32_t syscall_num = args->arg0;
    
    switch (syscall_num) {
        case SYS_READ: {
            // arg1 = device_id, arg2 = buffer, arg3 = size, arg4 = offset
            uint32_t device_id = args->arg1;
            void* buffer = (void*)args->arg2;
            size_t size = (size_t)args->arg3;
            uint32_t offset = args->arg4;
            
            return driver_read(device_id, buffer, size, offset);
        }
        
        case SYS_WRITE: {
            // arg1 = device_id, arg2 = buffer, arg3 = size, arg4 = offset
            uint32_t device_id = args->arg1;
            const void* buffer = (const void*)args->arg2;
            size_t size = (size_t)args->arg3;
            uint32_t offset = args->arg4;
            
            return driver_write(device_id, buffer, size, offset);
        }
        
        case SYS_OPEN: {
            // arg1 = filename (char*), arg2 = file_size (uint32_t*)
            const char* filename = (const char*)args->arg1;
            uint32_t* file_size = (uint32_t*)args->arg2;
            
            if (filename && file_size) {
                return fs_open(filename, file_size);
            }
            return -1;
        }
        
        case SYS_CLOSE: {
            // Пока не реализовано, просто возвращаем успех
            return 0;
        }
        
        case SYS_IOCTL: {
            // arg1 = device_id, arg2 = cmd, arg3 = arg
            uint32_t device_id = args->arg1;
            uint32_t cmd = args->arg2;
            void* arg = (void*)args->arg3;
            
            return driver_ioctl(device_id, cmd, arg);
        }
        
        case SYS_DEVICE_LIST: {
            // arg1 = type (driver_type), arg2 = devices (struct driver*), arg3 = max_count
            enum driver_type type = (enum driver_type)args->arg1;
            struct driver* devices = (struct driver*)args->arg2;
            int max_count = (int)args->arg3;
            
            return driver_list_by_type(type, devices, max_count);
        }
        
        case SYS_DEVICE_INFO: {
            // arg1 = device_id, arg2 = info (struct driver*)
            uint32_t device_id = args->arg1;
            struct driver* info = (struct driver*)args->arg2;
            
            struct driver* drv = driver_find_by_id(device_id);
            if (drv && info) {
                *info = *drv;
                return 0;
            }
            return -1;
        }

        case SYS_SOCKET: {
            return socket_create((int)args->arg1, (int)args->arg2, (int)args->arg3);
        }

        case SYS_BIND: {
            return socket_bind((int)args->arg1, (const struct sockaddr_in*)args->arg2);
        }

        case SYS_LISTEN: {
            return socket_listen((int)args->arg1, (int)args->arg2);
        }

        case SYS_ACCEPT: {
            return socket_accept((int)args->arg1, (int)args->arg2);
        }

        case SYS_CONNECT: {
            return socket_connect((int)args->arg1,
                                  (const struct sockaddr_in*)args->arg2,
                                  (int)args->arg3);
        }

        case SYS_SEND: {
            return socket_send((int)args->arg1, (const void*)args->arg2, (size_t)args->arg3);
        }

        case SYS_RECV: {
            return socket_recv((int)args->arg1, (void*)args->arg2, (size_t)args->arg3,
                               (int)args->arg4);
        }

        case SYS_SOCK_CLOSE: {
            return socket_close((int)args->arg1);
        }
        
        default:
            // Неизвестный системный вызов
            return -1;
    }
}

// Инициализация системы системных вызовов
void syscall_init() {
    // Обработчик уже зарегистрирован в IDT при idt_init()
    // Здесь можно добавить дополнительную инициализацию если нужно
}
