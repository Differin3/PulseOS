# Внутренний API ядра (kernel_api)

## Назначение
Унифицированный доступ к зарегистрированным драйверам из кода ядра без прямого вызова реализации драйвера. Обертки вокруг `driver_manager`.

## Функции (`kernel_api.h`)
- `kernel_device_read(device_id, buf, size, offset)` — чтение через `driver_read`.
- `kernel_device_write(device_id, buf, size, offset)` — запись через `driver_write`.
- `kernel_device_ioctl(device_id, cmd, arg)` — управление устройством через `driver_ioctl`.
- `kernel_device_list(type, drivers, max_count)` — получить список устройств типа `driver_type`.
- `kernel_device_get_info(device_id)` — получить указатель на `struct driver` по ID.
- `kernel_device_find_by_name(name)` — поиск по имени.
- `kernel_device_find_by_type(type, index)` — N-й драйвер указанного типа.

## Использование в ядре
```c
#include "kernel_api.h"

void example() {
  struct driver* d = kernel_device_find_by_type(DRIVER_STORAGE, 0);
  if (!d) return;
  char sector[512];
  if (kernel_device_read(d->device_id, sector, sizeof(sector), 0) == 512) {
    /* сектор прочитан */
  }
}
```

## Отличия от системных вызовов
- `kernel_api` вызывается только из кода ядра и не требует переключения контекста/прерывания.
- Системные вызовы (`int 0x80`) предназначены для пользовательских программ; внутри они маршрутизируются в `driver_manager` аналогично.

