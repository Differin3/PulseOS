#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fs.h"

#define RAMFS_MAX_NODES 64
#define RAMFS_NAME_MAX  32
#define RAMFS_FILE_MAX  4096

void ramfs_init(void);
int ramfs_mount_tmp(void);

int ramfs_lookup(const char* path, bool follow);
int ramfs_stat(const char* path, struct fs_stat* st);
int ramfs_read(const char* path, void* buf, size_t size);
int ramfs_write(const char* path, const void* data, size_t size);
int ramfs_list(const char* path, char* buf, size_t size);
int ramfs_mkdir(const char* path);
int ramfs_unlink(const char* path);
int ramfs_create_file(const char* path, uint16_t mode);
bool ramfs_is_tmp_path(const char* path);
void ramfs_strip_tmp(const char* path, char* out, size_t out_sz);

#endif
