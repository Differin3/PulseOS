#include "drivers/storage/nvme.h"
#include "drivers/video/terminal.h"
#include "kernel.h"
#include "driver_manager.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Локальный memset, чтобы не тянуть <string.h>
static void* nvme_memset(void* dest, int value, size_t num) {
    unsigned char* d = (unsigned char*)dest;
    for (size_t i = 0; i < num; i++) {
        d[i] = (unsigned char)value;
    }
    return dest;
}

// MMIO функции
static inline void mmio_write32(volatile uint32_t* addr, uint32_t val) {
    *addr = val;
}

static inline uint32_t mmio_read32(volatile uint32_t* addr) {
    return *addr;
}

static volatile uint32_t* nvme_regs = 0;
static bool nvme_initialized = false;
static uint32_t nvme_namespace_size = 0;
static uint32_t nvme_block_size = 512; // Стандартный размер блока

// Admin Queues
static struct nvme_sqe* admin_sq = 0;
static struct nvme_cqe* admin_cq = 0;
static uint16_t admin_sq_tail = 0;
static uint16_t admin_cq_head = 0;
static uint16_t admin_cq_phase = 1;
static uint16_t admin_cmd_id = 0;

// Вспомогательная функция для вывода hex числа
static void print_hex_nvme(uint32_t val) {
    const char hex_chars[] = "0123456789ABCDEF";
    terminal_writestring("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uint8_t nibble = (val >> i) & 0xF;
        char hex_char[2] = {hex_chars[nibble], 0};
        terminal_writestring(hex_char);
    }
}

// Функции-обертки для driver_manager
static int nvme_driver_read(void* device_data, void* buffer, size_t size, uint32_t offset) {
    (void)device_data;  // Не используется, используем глобальные переменные
    uint32_t lba = offset;
    size_t sectors = (size + 511) / 512;  // Округляем до секторов
    for (size_t i = 0; i < sectors; i++) {
        if (nvme_read_sector(lba + i, (uint8_t*)buffer + i * 512) != 0) {
            return -1;
        }
    }
    return size;
}

static int nvme_driver_write(void* device_data, const void* buffer, size_t size, uint32_t offset) {
    (void)device_data;
    uint32_t lba = offset;
    size_t sectors = (size + 511) / 512;
    for (size_t i = 0; i < sectors; i++) {
        if (nvme_write_sector(lba + i, (const uint8_t*)buffer + i * 512) != 0) {
            return -1;
        }
    }
    return size;
}

static int nvme_driver_ioctl(void* device_data, uint32_t cmd, void* arg) {
    (void)device_data;
    // IOCTL команды для NVMe
    // cmd = 0: получить размер диска
    if (cmd == 0 && arg) {
        uint32_t* size = (uint32_t*)arg;
        *size = nvme_get_disk_size();
        return 0;
    }
    return -1;
}

// Поиск NVMe контроллера через PCI
static volatile uint32_t* find_nvme_controller() {
    struct pci_device nvme_dev;
    // Класс 0x01 = Mass Storage, Подкласс 0x08 = NVM Express
    if (pci_find_device(0x01, 0x08, &nvme_dev) != 0) {
        return 0; // NVMe не найден
    }

    // Проверяем и при необходимости включаем MEMORY SPACE + BUS MASTER в PCI_COMMAND
    uint32_t cmd = pci_read_config(nvme_dev.bus, nvme_dev.device, nvme_dev.function, 0x04);
    uint32_t new_cmd = cmd | (1 << 1) | (1 << 2);
    if (new_cmd != cmd) {
        pci_write_config(nvme_dev.bus, nvme_dev.device, nvme_dev.function, 0x04, new_cmd);
    }
    
    // NVMe использует BAR0 для MMIO регистров
    if (nvme_dev.base_address[0] != 0) {
        return (volatile uint32_t*)(uintptr_t)nvme_dev.base_address[0];
    }
    
    return 0;
}

// Инициализация NVMe контроллера
int nvme_init() {
    if (nvme_initialized) return 0;
    
    // Ищем NVMe контроллер
    volatile uint32_t* base_addr = find_nvme_controller();
    if (!base_addr) {
        return -1; // контроллер не найден
    }
    
    nvme_regs = base_addr;
    
    // Проверяем доступность регистров
    uint32_t cap = mmio_read32(&nvme_regs[NVME_REG_CAP / 4]);
    if (cap == 0xFFFFFFFF) {
        terminal_writestring("[ERROR] NVMe: MMIO not accessible\n");
        return -1;
    }
    
    // Проверяем статус контроллера
    uint32_t csts = mmio_read32(&nvme_regs[NVME_REG_CSTS / 4]);
    if (csts == 0xFFFFFFFF) {
        terminal_writestring("[ERROR] NVMe: MMIO not accessible\n");
        return -1;
    }
    
    // Проверяем, готов ли контроллер
    if (!(csts & NVME_CSTS_RDY)) {
        // Сначала отключаем контроллер, если он был включен
        uint32_t cc = mmio_read32(&nvme_regs[NVME_REG_CC / 4]);
        if (cc & NVME_CC_EN) {
            cc &= ~NVME_CC_EN;
            mmio_write32(&nvme_regs[NVME_REG_CC / 4], cc);
            // Ждем отключения
            for (int i = 0; i < 1000; i++) {
                csts = mmio_read32(&nvme_regs[NVME_REG_CSTS / 4]);
                if (!(csts & NVME_CSTS_RDY)) break;
            }
        }
        
        // Включаем контроллер (устанавливаем CC.EN = 1)
        cc = mmio_read32(&nvme_regs[NVME_REG_CC / 4]);
        cc = (cc & ~0xFFFF0000) | (0x04 << NVME_CC_IOSQES_SHIFT) | (0x04 << NVME_CC_IOCQES_SHIFT) | NVME_CC_EN;
        mmio_write32(&nvme_regs[NVME_REG_CC / 4], cc);
        
        // Ждем готовности контроллера (увеличиваем таймаут)
        bool ready = false;
        for (int i = 0; i < 10000; i++) {
            csts = mmio_read32(&nvme_regs[NVME_REG_CSTS / 4]);
            if (csts & NVME_CSTS_RDY) {
                ready = true;
                break;
            }
        }
        
        if (!ready) {
            terminal_writestring("[ERROR] NVMe: Controller failed to become ready (CSTS=");
            print_hex_nvme(csts);
            terminal_writestring(")\n");
            return -1;
        }
    }
    
    // Выделяем память для Admin Queues
    static uint8_t admin_sq_buffer[NVME_ADMIN_QUEUE_SIZE * NVME_SQ_ENTRY_SIZE] __attribute__((aligned(4096)));
    static uint8_t admin_cq_buffer[NVME_ADMIN_QUEUE_SIZE * NVME_CQ_ENTRY_SIZE] __attribute__((aligned(4096)));
    
    admin_sq = (struct nvme_sqe*)admin_sq_buffer;
    admin_cq = (struct nvme_cqe*)admin_cq_buffer;
    
    // Инициализируем очереди
    nvme_memset(admin_sq, 0, NVME_ADMIN_QUEUE_SIZE * NVME_SQ_ENTRY_SIZE);
    nvme_memset(admin_cq, 0, NVME_ADMIN_QUEUE_SIZE * NVME_CQ_ENTRY_SIZE);
    
    // Настраиваем Admin Queue Attributes (AQA)
    uint32_t aqa = ((NVME_ADMIN_QUEUE_SIZE - 1) << 16) | (NVME_ADMIN_QUEUE_SIZE - 1);
    mmio_write32(&nvme_regs[NVME_REG_AQA / 4], aqa);
    
    // Настраиваем Admin Submission Queue Base Address (ASQ)
    uint64_t asq_phys = (uint64_t)(uintptr_t)admin_sq;
    mmio_write32(&nvme_regs[NVME_REG_ASQ / 4], (uint32_t)asq_phys);
    mmio_write32(&nvme_regs[(NVME_REG_ASQ + 4) / 4], (uint32_t)(asq_phys >> 32));
    
    // Настраиваем Admin Completion Queue Base Address (ACQ)
    uint64_t acq_phys = (uint64_t)(uintptr_t)admin_cq;
    mmio_write32(&nvme_regs[NVME_REG_ACQ / 4], (uint32_t)acq_phys);
    mmio_write32(&nvme_regs[(NVME_REG_ACQ + 4) / 4], (uint32_t)(acq_phys >> 32));
    
    // Проверяем, что адреса установлены правильно
    uint32_t asq_low = mmio_read32(&nvme_regs[NVME_REG_ASQ / 4]);
    uint32_t acq_low = mmio_read32(&nvme_regs[NVME_REG_ACQ / 4]);
    (void)mmio_read32(&nvme_regs[(NVME_REG_ASQ + 4) / 4]);
    (void)mmio_read32(&nvme_regs[(NVME_REG_ACQ + 4) / 4]);
    
    if (asq_low != (uint32_t)asq_phys || acq_low != (uint32_t)acq_phys) {
        terminal_writestring("[ERROR] NVMe: Queue addresses not set correctly\n");
        return -1;
    }
    
    // Упрощенная версия: используем namespace 1 и предполагаем размер блока 512 байт
    nvme_namespace_size = 4194304; // ~2GB в секторах по 512 байт
    nvme_block_size = 512;
    
    nvme_initialized = true;
    
    // Регистрируем драйвер в менеджере драйверов
    struct driver nvme_driver;
    nvme_driver.name[0] = 'n';
    nvme_driver.name[1] = 'v';
    nvme_driver.name[2] = 'm';
    nvme_driver.name[3] = 'e';
    nvme_driver.name[4] = '0';
    nvme_driver.name[5] = 0;
    nvme_driver.type = DRIVER_STORAGE;
    nvme_driver.device_id = 0;  // Будет присвоен автоматически
    nvme_driver.device_data = (void*)nvme_regs;  // Сохраняем указатель на регистры
    nvme_driver.initialized = true;
    nvme_driver.active = true;
    
    // Реализуем операции драйвера
    nvme_driver.ops.init = 0;  // Уже инициализирован
    nvme_driver.ops.read = nvme_driver_read;
    nvme_driver.ops.write = nvme_driver_write;
    nvme_driver.ops.ioctl = nvme_driver_ioctl;
    nvme_driver.ops.cleanup = 0;  // Пока не нужна
    
    if (driver_register(&nvme_driver) != 0) {
        terminal_writestring("[WARNING] NVMe: Failed to register driver in driver_manager\n");
    }
    
    return 0;
}

// Отправка команды в Admin Queue (упрощенная версия)
static int nvme_submit_admin_cmd(uint8_t opcode, uint32_t nsid, uint64_t prp1, uint64_t prp2, uint32_t cdw10, uint32_t cdw11) {
    if (!nvme_initialized) return -1;
    
    // Заполняем Submission Queue Entry
    struct nvme_sqe* sqe = &admin_sq[admin_sq_tail];
    nvme_memset(sqe, 0, sizeof(struct nvme_sqe));
    
    sqe->cdw0 = opcode | (admin_cmd_id << 16); // Command ID
    admin_cmd_id = (admin_cmd_id + 1) % 65536;
    sqe->cdw1 = nsid;
    sqe->prp1 = prp1;
    sqe->prp2 = prp2;
    sqe->cdw10 = cdw10;
    sqe->cdw11 = cdw11;
    
    // Обновляем tail pointer (Admin SQ Tail Doorbell = 0x1000)
    admin_sq_tail = (admin_sq_tail + 1) % NVME_ADMIN_QUEUE_SIZE;
    mmio_write32(&nvme_regs[0x1000 / 4], admin_sq_tail);
    
    // Ждем завершения команды (упрощенная версия)
    for (int i = 0; i < 10000; i++) {
        struct nvme_cqe* cqe = &admin_cq[admin_cq_head];
        uint16_t phase = (cqe->status >> 1) & 1;
        
        if (phase == admin_cq_phase) {
            // Команда завершена
            uint16_t status = cqe->status & 0xFFFE;
            if (status != 0) {
                return -1; // Ошибка
            }
            
            // Обновляем head pointer (Admin CQ Head Doorbell = 0x1004)
            admin_cq_head = (admin_cq_head + 1) % NVME_ADMIN_QUEUE_SIZE;
            if (admin_cq_head == 0) {
                admin_cq_phase = !admin_cq_phase;
            }
            mmio_write32(&nvme_regs[0x1004 / 4], admin_cq_head);
            
            return 0;
        }
    }
    
    return -1; // Timeout
}

// Чтение сектора через NVMe (упрощенная версия - через Admin Queue)
int nvme_read_sector(uint32_t lba, void* buffer) {
    if (!nvme_initialized) return -1;
    if (!buffer) return -1; // проверка на NULL указатель
    
    // Упрощенная версия: используем прямую запись в буфер, без I/O очередей
    uint64_t prp1 = (uint64_t)(uintptr_t)buffer;
    
    // LBA считаем 32-битным: старшие 32 бита = 0, чтобы не было сдвига на 32 бита
    return nvme_submit_admin_cmd(NVME_CMD_READ, 1, prp1, 0, (uint32_t)lba, 0);
}

// Запись сектора через NVMe (упрощенная версия)
int nvme_write_sector(uint32_t lba, const void* buffer) {
    if (!nvme_initialized) return -1;
    if (!buffer) return -1; // проверка на NULL указатель
    
    uint64_t prp1 = (uint64_t)(uintptr_t)buffer;
    
    // Аналогично чтению: используем только младшие 32 бита LBA
    return nvme_submit_admin_cmd(NVME_CMD_WRITE, 1, prp1, 0, (uint32_t)lba, 0);
}

// Получить размер диска в секторах
uint32_t nvme_get_disk_size() {
    if (!nvme_initialized) return 0;
    return nvme_namespace_size;
}
