#ifndef MOUNT_H
#define MOUNT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_MOUNTS 8
#define FS_TYPE_MOS   0
#define FS_TYPE_RAMFS 1

struct mount_point {
    char path[64];
    int disk_id;
    int partition_id;
    uint8_t fs_type;
    bool active;
} __attribute__((packed));

int mount_fs(int disk_id, const char* path);
int mount_register(const char* path, int disk_id, uint8_t fs_type);
int unmount_fs(const char* path);
const struct mount_point* get_mount_for_path(const char* path);
int mount_init(void);
int mount_count_active(void);
const struct mount_point* mount_get(int index);

#endif
