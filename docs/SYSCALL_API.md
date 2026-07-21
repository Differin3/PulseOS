# Системные вызовы (int 0x80)

## Общие сведения
- Прерывание: `int 0x80`.
- Регистры при вызове:
  - `eax` — номер вызова.
  - `ebx` — arg1, `ecx` — arg2, `edx` — arg3, `esi` — arg4.
- Возврат: `eax` — код возврата (`0` или `>=0` успех, `<0` ошибка).
- Структура аргументов в ядре: `struct syscall_args { arg0..arg4 }` (`syscall.h`).

## Доступные вызовы
- `SYS_READ (0)` — чтение из устройства.
  - args: `device_id`, `buffer`, `size`, `offset`.
  - offset для блочных устройств трактуется как LBA.
- `SYS_WRITE (1)` — запись в устройство.
  - args: `device_id`, `buffer`, `size`, `offset`.
- `SYS_OPEN (2)` — открыть файл (простейший интерфейс).
  - args: `filename (char*)`, `file_size (uint32_t*)`.
- `SYS_CLOSE (3)` — не реализован (заглушка). Всегда возвращает 0. Таблица открытых файловых дескрипторов пока отсутствует.
- `SYS_IOCTL (4)` — управление устройством.
  - args: `device_id`, `cmd`, `arg`.
  - cmd/arg определяются драйвером; для storage cmd=0 возвращает размер диска (uint32_t*).
- `SYS_DEVICE_LIST (5)` — получить список устройств указанного типа.
  - args: `driver_type`, `struct driver* out`, `max_count`.
  - возвращает количество записанных элементов.
- `SYS_DEVICE_INFO (6)` — получить информацию об устройстве по ID.
  - args: `device_id`, `struct driver* info`.
  - возвращает 0/ -1.

### Сокеты (Socket API)

- `SYS_SOCKET (7)` — создать сокет.
  - args: `domain` (AF_INET=2), `type` (SOCK_STREAM=1 / SOCK_DGRAM=2), `protocol` (0).
  - возвращает fd (0..31) или -1.
- `SYS_BIND (8)` — привязать локальный порт.
  - args: `fd`, `struct sockaddr_in*`.
  - `sockaddr_in`: `{ sin_family, sin_port, sin_addr }` (порт в host byte order).
- `SYS_LISTEN (9)` — слушать TCP (только SOCK_STREAM).
  - args: `fd`, `backlog`.
- `SYS_ACCEPT (10)` — принять TCP-соединение.
  - args: `fd`, `timeout_ms`.
  - возвращает новый fd клиента или -1.
- `SYS_CONNECT (11)` — подключиться (TCP) или задать peer (UDP).
  - args: `fd`, `struct sockaddr_in*`, `timeout_ms`.
- `SYS_SEND (12)` — отправить данные.
  - args: `fd`, `buffer`, `len`.
- `SYS_RECV (13)` — принять данные.
  - args: `fd`, `buffer`, `len`, `timeout_ms`.
  - возвращает число байт, 0 при таймауте, -1 при ошибке.
- `SYS_SOCK_CLOSE (14)` — закрыть сокет.
  - args: `fd`.

Перед сетевыми вызовами нужен IP (`dhcp` или `network static`). В shell: `socktest tcp|udp <port>`.

## Примеры вызова (ассемблер userland)
```asm
; Пример: прочитать 1 сектор с LBA0 в буфер buf
mov eax, 0          ; SYS_READ
mov ebx, 1          ; device_id (например nvme0/ahci0)
mov ecx, buf        ; buffer
mov edx, 512        ; size
mov esi, 0          ; offset (LBA)
int 0x80
; eax = результат (512 при успехе, <0 при ошибке)
```

```asm
; Пример: TCP connect + send (упрощённо)
; sockaddr_in: family=2, port=80, addr=0x0100007F (127.0.0.1)
mov eax, 7          ; SYS_SOCKET
mov ebx, 2          ; AF_INET
mov ecx, 1          ; SOCK_STREAM
mov edx, 0
int 0x80
; eax = fd

mov eax, 11         ; SYS_CONNECT
mov ebx, eax        ; fd (сохранить fd отдельно в реальном коде)
mov ecx, sockaddr
mov edx, 5000       ; timeout_ms
int 0x80
```

```asm
; Пример: получить список storage устройств (до 4 штук)
mov eax, 5          ; SYS_DEVICE_LIST
mov ebx, 0          ; DRIVER_STORAGE
mov ecx, devices    ; struct driver out[]
mov edx, 4          ; max_count
int 0x80
; eax = число найденных устройств
```

## Обработчик в ядре
- Ассемблер: `boot/interrupts.asm` — сохраняет регистры, формирует `syscall_args`, вызывает `syscall_handler`.
- Ядро: `kernel/syscall.cpp` — маршрутизирует вызовы и использует `driver_manager` / `fs`.

