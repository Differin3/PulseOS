#ifndef NVME_H
#define NVME_H

#include <stdint.h>
#include "drivers/pci/pci.h"

// NVMe регистры контроллера (offset от BAR0)
#define NVME_REG_CAP      0x00  // Capabilities
#define NVME_REG_VS       0x08  // Version
#define NVME_REG_CC       0x14  // Controller Configuration
#define NVME_REG_CSTS     0x1C  // Controller Status
#define NVME_REG_AQA      0x24  // Admin Queue Attributes
#define NVME_REG_ASQ      0x28  // Admin Submission Queue Base Address
#define NVME_REG_ACQ      0x30  // Admin Completion Queue Base Address

// NVMe команды
#define NVME_CMD_IDENTIFY        0x06  // Identify Controller/Namespace
#define NVME_CMD_READ            0x02  // Read
#define NVME_CMD_WRITE           0x01  // Write

// NVME Status Register (CSTS) биты
#define NVME_CSTS_RDY            (1 << 0)  // Ready
#define NVME_CSTS_CFS            (1 << 1)  // Controller Fatal Status

// NVME CC Register биты
#define NVME_CC_EN               (1 << 0)  // Enable
#define NVME_CC_IOSQES_SHIFT     16        // I/O Submission Queue Entry Size
#define NVME_CC_IOCQES_SHIFT     20        // I/O Completion Queue Entry Size

// Размеры структур NVMe
#define NVME_SQ_ENTRY_SIZE       64        // Submission Queue Entry
#define NVME_CQ_ENTRY_SIZE       16        // Completion Queue Entry
#define NVME_ADMIN_QUEUE_SIZE    256       // Размер Admin Queue

// Submission Queue Entry (SQE)
struct nvme_sqe {
    uint32_t cdw0;      // Command Dword 0 (Opcode, Fused Operation, etc.)
    uint32_t cdw1;      // Command Dword 1 (Namespace ID)
    uint64_t prp1;      // Physical Region Page 1
    uint64_t prp2;      // Physical Region Page 2
    uint64_t metadata;  // Metadata Pointer
    uint32_t cdw10;     // Command Dword 10
    uint32_t cdw11;     // Command Dword 11
    uint32_t cdw12;     // Command Dword 12
    uint32_t cdw13;     // Command Dword 13
    uint32_t cdw14;     // Command Dword 14
    uint32_t cdw15;     // Command Dword 15
} __attribute__((packed));

// Completion Queue Entry (CQE)
struct nvme_cqe {
    uint32_t result;    // Command Specific Result
    uint32_t rsvd;      // Reserved
    uint16_t sq_head;   // Submission Queue Head Pointer
    uint16_t sq_id;     // Submission Queue Identifier
    uint16_t command_id;// Command Identifier
    uint16_t status;    // Status Field
} __attribute__((packed));

// Инициализация NVMe контроллера
int nvme_init();

// Чтение сектора через NVMe
int nvme_read_sector(uint32_t lba, void* buffer);

// Запись сектора через NVMe
int nvme_write_sector(uint32_t lba, const void* buffer);

// Получить размер диска в секторах
uint32_t nvme_get_disk_size();

#endif
