#include "drivers/storage/disk_manager.h"
#include "drivers/storage/ata.h"
#include "drivers/storage/ahci.h"
#include "drivers/storage/nvme.h"
#include "driver_manager.h"
#include "kernel_api.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

static struct disk_info disk_list[MAX_DISKS];
static int disk_count = 0;
static bool ahci_pci_found = false;
static bool ahci_init_attempted = false;

static void disk_set_name(struct disk_info* disk, int index) {
    disk->name[0] = 's';
    disk->name[1] = 'd';
    disk->name[2] = 'a' + (char)index;
    disk->name[3] = 0;
}

static void disk_init_fields(struct disk_info* disk) {
    disk->ata_lba48 = false;
    disk->ata_channel = DISK_LOC_NA;
    disk->ata_select = DISK_LOC_NA;
    disk->ahci_port = DISK_LOC_NA;
}

int disk_manager_init() {
    disk_count = 0;

    struct driver storage_drivers[8];
    int storage_count = kernel_device_list(DRIVER_STORAGE, storage_drivers, 8);

    for (int i = 0; i < storage_count && disk_count < MAX_DISKS; i++) {
        struct driver* drv = &storage_drivers[i];

        enum disk_controller_type ctrl_type = DISK_CONTROLLER_NONE;
        if (drv->name[0] == 'n' && drv->name[1] == 'v' && drv->name[2] == 'm' && drv->name[3] == 'e') {
            ctrl_type = DISK_CONTROLLER_NVME;
        } else if (drv->name[0] == 'a' && drv->name[1] == 'h' && drv->name[2] == 'c' && drv->name[3] == 'i') {
            ctrl_type = DISK_CONTROLLER_AHCI;
            ahci_pci_found = true;
            ahci_init_attempted = true;
        } else if (drv->name[0] == 'a' && drv->name[1] == 't' && drv->name[2] == 'a') {
            continue;
        }
        if (ctrl_type == DISK_CONTROLLER_NONE) continue;

        uint32_t disk_size = 0;
        if (drv->ops.ioctl) {
            drv->ops.ioctl(drv->device_data, 0, &disk_size);
        }
        if (disk_size == 0 && ctrl_type == DISK_CONTROLLER_NVME) {
            disk_size = 4194304;
        }

        struct disk_info* d = &disk_list[disk_count];
        disk_init_fields(d);
        d->id = disk_count;
        d->controller_type = ctrl_type;
        d->size_sectors = disk_size;
        d->initialized = drv->initialized;
        d->active = drv->active;
        d->is_atapi = false;

        if (ctrl_type == DISK_CONTROLLER_AHCI && drv->device_data) {
            struct ahci_port_state* port = (struct ahci_port_state*)drv->device_data;
            d->ahci_port = ahci_port_number(port);
            if (disk_size == 0) d->size_sectors = ahci_port_disk_size(port);
        }

        int j = 0;
        while (drv->name[j] && j < 15) {
            d->name[j] = drv->name[j];
            j++;
        }
        d->name[j] = 0;

        disk_count++;
    }

    struct ata_probed_device probed[ATA_MAX_PROBED];
    int ata_found = ata_probe_all(probed, ATA_MAX_PROBED);
    for (int i = 0; i < ata_found && disk_count < MAX_DISKS; i++) {
        bool already = false;
        for (int j = 0; j < disk_count; j++) {
            if (disk_list[j].controller_type == DISK_CONTROLLER_ATA &&
                disk_list[j].ata_channel == probed[i].channel &&
                disk_list[j].ata_select == probed[i].select) {
                already = true;
                break;
            }
        }
        if (already) continue;

        struct disk_info* d = &disk_list[disk_count];
        disk_init_fields(d);
        d->id = disk_count;
        d->controller_type = DISK_CONTROLLER_ATA;
        d->ata_channel = probed[i].channel;
        d->ata_select = probed[i].select;
        d->is_atapi = probed[i].is_atapi;
        d->ata_lba48 = probed[i].lba48;
        d->size_sectors = probed[i].is_atapi ? 0 : probed[i].size_sectors;
        d->initialized = !probed[i].is_atapi && probed[i].size_sectors > 0;
        d->active = false;
        disk_set_name(d, disk_count);
        disk_count++;
    }

    return disk_count;
}

const struct disk_info* disk_manager_get_disk(int disk_id) {
    if (disk_id < 0 || disk_id >= disk_count) return 0;
    return &disk_list[disk_id];
}

struct disk_info* disk_manager_get_disk_mutable(int disk_id) {
    if (disk_id < 0 || disk_id >= disk_count) return 0;
    return &disk_list[disk_id];
}

int disk_manager_list(struct disk_info* disks, int max_count) {
    int count = disk_count < max_count ? disk_count : max_count;
    for (int i = 0; i < count; i++) disks[i] = disk_list[i];
    return count;
}

int disk_manager_count() {
    return disk_count;
}

bool disk_manager_ahci_pci_found() {
    return ahci_pci_found;
}

bool disk_manager_ahci_init_attempted() {
    return ahci_init_attempted;
}

int disk_select(int disk_id) {
    if (disk_id < 0 || disk_id >= disk_count) return -1;

    const struct disk_info* disk = &disk_list[disk_id];
    if (!disk->initialized) return -1;

    disk_set_controller(disk->controller_type);

    if (disk->controller_type == DISK_CONTROLLER_ATA &&
        disk->ata_channel != DISK_LOC_NA) {
        struct ata_device_id dev;
        dev.channel = disk->ata_channel;
        dev.select = disk->ata_select;
        dev.lba48 = disk->ata_lba48;
        dev.is_atapi = disk->is_atapi;
        dev.size_sectors = disk->size_sectors;
        ata_select_device(&dev);
    } else if (disk->controller_type == DISK_CONTROLLER_AHCI &&
               disk->ahci_port != DISK_LOC_NA) {
        for (int p = 0; p < ahci_port_count(); p++) {
            struct ahci_port_state* port = ahci_get_port(p);
            if (port && ahci_port_number(port) == disk->ahci_port) {
                ahci_select_port(port);
                break;
            }
        }
    }

    for (int i = 0; i < disk_count; i++) {
        disk_list[i].active = (i == disk_id);
    }
    return 0;
}
