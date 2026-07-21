# Менеджер драйверов

## Архитектура
- Единый список драйверов: `driver_manager.cpp`, максимум 32 элементов.
- Базовая сущность `struct driver` (`driver_manager.h`): имя, тип (`driver_type`), `device_id`, `device_data`, набор операций `driver_ops`, флаги `initialized/active`.
- Категории драйверов: `DRIVER_STORAGE`, `DRIVER_INPUT`, `DRIVER_VIDEO`, `DRIVER_NETWORK`, `DRIVER_OTHER`.
- Жизненный цикл:
  1. Ядро вызывает `driver_manager_init()`.
  2. Драйвер инициализируется и регистрируется через `driver_register()`.
  3. Устройство становится доступно через унифицированные вызовы `driver_read/write/ioctl`.

## API для разработчиков драйверов
- Реализовать набор операций `struct driver_ops` (нужны только те, что поддерживает устройство).
  - `init(void* device_data)` — необязательно, если инициализация уже выполнена.
  - `read(void* device_data, void* buf, size_t size, uint32_t offset)` — чтение (для блочных устройств offset трактуется как LBA).
  - `write(void* device_data, const void* buf, size_t size, uint32_t offset)` — запись.
  - `ioctl(void* device_data, uint32_t cmd, void* arg)` — управление устройством (возврат 0 при успехе, -1 при ошибке).
  - `cleanup(void* device_data)` — освобождение ресурсов (по необходимости).
- Зарегистрировать драйвер:
```c
struct driver mydrv = {
  .name = "foo0",
  .type = DRIVER_OTHER,
  .device_id = 0,            // 0 -> автоназначение
  .device_data = ctx_ptr,    // приватные данные драйвера
  .ops = {
    .init = 0,
    .read = my_read,
    .write = my_write,
    .ioctl = my_ioctl,
    .cleanup = 0,
  },
  .initialized = true,
  .active = true,
};
driver_register(&mydrv);
```

## API для использования драйверов
- Поиск:
  - `driver_find_by_type(type, index)` — получить N-й драйвер указанного типа.
  - `driver_find_by_name(name)` — поиск по имени.
  - `driver_find_by_id(device_id)` — поиск по ID.
  - `driver_list_by_type(type, out[], max)` — вернуть список.
- Операции с устройством:
  - `driver_read(device_id, buf, size, offset)`
  - `driver_write(device_id, buf, size, offset)`
  - `driver_ioctl(device_id, cmd, arg)`

## Примеры
### Регистрация простого драйвера ввода
```c
static int kbd_ioctl(void* dev, uint32_t cmd, void* arg) { /* ... */ }

void keyboard_init() {
  /* аппаратная инициализация */
  struct driver kbd = {
    .name = "kbd0",
    .type = DRIVER_INPUT,
    .device_id = 0,
    .device_data = 0,
    .ops = { .init=0, .read=keyboard_driver_read, .write=keyboard_driver_write, .ioctl=kbd_ioctl, .cleanup=0 },
    .initialized = true,
    .active = true,
  };
  driver_register(&kbd);
}
```

### Доступ к первому блочному устройству
```c
struct driver* d = driver_find_by_type(DRIVER_STORAGE, 0);
if (d) {
  char sector[512];
  driver_read(d->device_id, sector, sizeof(sector), /*offset=LBA*/0);
}
```

## Драйверы хранения (ATA / AHCI)

### Схема
- `driver_scan_devices()` — PCI scan: NVMe, затем AHCI (`ahci_init_with_device`).
- `disk_init()` — выбор активного контроллера: NVMe → AHCI → IDE PIO.
- `disk_manager_init()` — список дисков из зарегистрированных драйверов + `ata_probe_all()` для IDE.
- `disk_select(id)` — переключает контроллер и маршрутизирует на канал IDE / порт AHCI.

### AHCI (`ahci0`, `ahci1`, …)
- Per-port state: command list, received FIS, 32 command tables (~8 KB/порт).
- DMA EXT (`0x25`/`0x35`) с fallback PIO (`0x20`/`0x30`), write-bit, TFD wait, PxIS clear.
- Intel ICH7/8/9: PCI config `0x90 |= 0x40`.
- `device_data` → `struct ahci_port_state`; IOCTL cmd=0 → размер в секторах.

### IDE (`ata.cpp`)
- Каналы: primary `0x1F0`, secondary `0x170`; master `0xE0`, slave `0xF0`.
- LBA28/LBA48 PIO (`0x24`/`0x34`), flush cache `0xE7` после записи.
- `ata_probe_all()` — обнаружение устройств без дублирования в `disk_manager`.

### QEMU
| Скрипт | Назначение |
|--------|------------|
| `run-qemu.bat` | ISO only (без отдельного диска) |
| `run-qemu-ahci.bat` | `ich9-ahci` + `myos.img` |
| `run-qemu-ide.bat` | legacy IDE + `myos.img` |

### Ограничения v1
- NCQ, hot-plug, ATAPI read — не реализованы.
- Максимум 2 активных AHCI-порта; LBA в API — `uint32_t` (до ~2 TB).
