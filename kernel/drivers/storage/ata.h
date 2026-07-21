#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ATA_PRIMARY_DATA         0x1F0
#define ATA_PRIMARY_ERROR        0x1F1
#define ATA_PRIMARY_SECTOR_COUNT 0x1F2
#define ATA_PRIMARY_LBA_LOW      0x1F3
#define ATA_PRIMARY_LBA_MID      0x1F4
#define ATA_PRIMARY_LBA_HIGH     0x1F5
#define ATA_PRIMARY_DEVICE       0x1F6
#define ATA_PRIMARY_COMMAND      0x1F7
#define ATA_PRIMARY_STATUS       0x1F7
#define ATA_PRIMARY_CONTROL      0x3F6

#define ATA_SECONDARY_DATA       0x170
#define ATA_SECONDARY_CONTROL    0x376

#define ATA_CMD_READ_SECTORS     0x20
#define ATA_CMD_WRITE_SECTORS    0x30
#define ATA_CMD_READ_PIO_EXT     0x24
#define ATA_CMD_WRITE_PIO_EXT    0x34
#define ATA_CMD_READ_DMA_EXT     0x25
#define ATA_CMD_WRITE_DMA_EXT    0x35
#define ATA_CMD_IDENTIFY         0xEC
#define ATA_CMD_FLUSH_CACHE      0xE7

#define ATA_SELECT_MASTER        0xE0
#define ATA_SELECT_SLAVE         0xF0

#define ATA_STATUS_ERR           0x01
#define ATA_STATUS_DRQ           0x08
#define ATA_STATUS_BSY           0x80

#define ATA_MAX_PROBED           8
#define ATA_CHANNEL_INVALID      0xFF

enum disk_controller_type {
    DISK_CONTROLLER_NONE = 0,
    DISK_CONTROLLER_ATA = 1,
    DISK_CONTROLLER_AHCI = 2,
    DISK_CONTROLLER_NVME = 3
};

struct ata_channel {
    uint16_t io_base;
    uint16_t ctrl_base;
};

struct ata_device_id {
    uint8_t channel;
    uint8_t select;
    bool lba48;
    bool is_atapi;
    uint32_t size_sectors;
};

struct ata_probed_device {
    uint8_t channel;
    uint8_t select;
    bool is_atapi;
    bool lba48;
    uint32_t size_sectors;
};

int disk_init();
enum disk_controller_type disk_get_controller_type();
const char* disk_get_controller_name();
void disk_set_controller(enum disk_controller_type type);

int ata_init();
int ata_init_device(const struct ata_device_id* dev);
void ata_select_device(const struct ata_device_id* dev);
const struct ata_device_id* ata_get_selected_device();
int ata_probe_all(struct ata_probed_device* out, int max_count);

int disk_read_sector(uint32_t lba, void* buffer);
int disk_write_sector(uint32_t lba, const void* buffer);
int disk_read_sectors(uint32_t lba, size_t count, void* buffer);
int disk_write_sectors(uint32_t lba, size_t count, const void* buffer);

int ata_read_sector(uint32_t lba, void* buffer);
int ata_write_sector(uint32_t lba, const void* buffer);
int ata_read_sectors(uint32_t lba, size_t count, void* buffer);
int ata_write_sectors(uint32_t lba, size_t count, const void* buffer);
int ata_read_sectors_dev(const struct ata_device_id* dev, uint32_t lba, size_t count, void* buf);
int ata_write_sectors_dev(const struct ata_device_id* dev, uint32_t lba, size_t count, const void* buf);

uint32_t disk_get_size_sectors();
uint32_t ata_device_size_sectors(const struct ata_device_id* dev);

#endif
