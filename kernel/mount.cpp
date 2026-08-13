#include "mount.h"
#include "drivers/storage/disk_manager.h"
#include <string.h>

static struct mount_point mount_table[MAX_MOUNTS];
static int mount_count = 0;

int mount_init(void) {
    mount_count = 0;
    if (disk_manager_count() > 0) {
        disk_select(0);
        mount_table[0].disk_id = 0;
        mount_table[0].partition_id = 0;
        mount_table[0].fs_type = FS_TYPE_MOS;
        mount_table[0].active = true;
        mount_table[0].path[0] = '/';
        mount_table[0].path[1] = 0;
        mount_count = 1;
    }
    return mount_count;
}

int mount_register(const char* path, int disk_id, uint8_t fs_type) {
    if (!path || mount_count >= MAX_MOUNTS) return -1;
    for (int i = 0; i < mount_count; i++) {
        if (!mount_table[i].active) continue;
        if (strcmp(mount_table[i].path, path) == 0) {
            mount_table[i].disk_id = disk_id;
            mount_table[i].fs_type = fs_type;
            return 0;
        }
    }
    int i = 0;
    while (path[i] && i < 63) {
        mount_table[mount_count].path[i] = path[i];
        i++;
    }
    mount_table[mount_count].path[i] = 0;
    mount_table[mount_count].disk_id = disk_id;
    mount_table[mount_count].partition_id = 0;
    mount_table[mount_count].fs_type = fs_type;
    mount_table[mount_count].active = true;
    mount_count++;
    return 0;
}

int mount_fs(int disk_id, const char* path) {
    if (disk_id < 0 || disk_id >= disk_manager_count()) return -1;
    if (disk_select(disk_id) != 0) return -1;
    for (int i = 0; i < mount_count; i++) {
        if (mount_table[i].disk_id == disk_id && mount_table[i].active &&
            mount_table[i].fs_type == FS_TYPE_MOS)
            return -1;
    }
    return mount_register(path, disk_id, FS_TYPE_MOS);
}

int unmount_fs(const char* path) {
    for (int i = 0; i < mount_count; i++) {
        if (!mount_table[i].active) continue;
        if (strcmp(mount_table[i].path, path) == 0) {
            if (mount_table[i].path[0] == '/' && mount_table[i].path[1] == 0) return -1;
            mount_table[i].active = false;
            return 0;
        }
    }
    return -1;
}

const struct mount_point* get_mount_for_path(const char* path) {
    const struct mount_point* best_match = 0;
    int best_match_len = 0;
    if (!path) path = "/";
    for (int i = 0; i < mount_count; i++) {
        if (!mount_table[i].active) continue;
        int j = 0;
        while (path[j] && mount_table[i].path[j] && path[j] == mount_table[i].path[j]) j++;
        if (!mount_table[i].path[j]) {
            /* match if path ends or next char is '/' (unless mount is "/") */
            if (mount_table[i].path[0] == '/' && mount_table[i].path[1] == 0) {
                if (j > best_match_len) {
                    best_match = &mount_table[i];
                    best_match_len = j;
                }
            } else if (path[j] == 0 || path[j] == '/') {
                if (j > best_match_len) {
                    best_match = &mount_table[i];
                    best_match_len = j;
                }
            }
        }
    }
    return best_match ? best_match : (mount_count ? &mount_table[0] : 0);
}

int mount_count_active(void) {
    int n = 0;
    for (int i = 0; i < mount_count; i++)
        if (mount_table[i].active) n++;
    return n;
}

const struct mount_point* mount_get(int index) {
    int n = 0;
    for (int i = 0; i < mount_count; i++) {
        if (!mount_table[i].active) continue;
        if (n == index) return &mount_table[i];
        n++;
    }
    return 0;
}
