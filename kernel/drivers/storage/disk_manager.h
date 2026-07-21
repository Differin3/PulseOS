#ifndef DISK_MANAGER_H
#define DISK_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/storage/ata.h"

#define MAX_DISKS 8
#define DISK_LOC_NA 0xFF

struct disk_info {
    int id;
    enum disk_controller_type controller_type;
    uint32_t size_sectors;
    bool initialized;
    bool active;
    bool is_atapi;
    bool ata_lba48;
    uint8_t ata_channel;
    uint8_t ata_select;
    uint8_t ahci_port;
    char name[16];
} __attribute__((packed));

int disk_manager_init();
const struct disk_info* disk_manager_get_disk(int disk_id);
struct disk_info* disk_manager_get_disk_mutable(int disk_id);
int disk_manager_list(struct disk_info* disks, int max_count);
int disk_manager_count();
bool disk_manager_ahci_pci_found();
bool disk_manager_ahci_init_attempted();
int disk_select(int disk_id);

#endif
