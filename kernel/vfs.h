#ifndef VFS_H
#define VFS_H

#include "fs.h"
#include "mount.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct vfs_node {
    const struct mount_point* mount;
    int mos_index; /* -2 = root dir, -1 = miss, >=0 file table idx */
};

/* Resolve path relative to task cwd when not absolute. */
int vfs_resolve(const char* path, bool follow_symlinks, struct vfs_node* out);

int vfs_stat(const char* path, struct fs_stat* st);
int vfs_read(const char* path, void* buf, size_t size);
int vfs_write(const char* path, const void* data, size_t size);
int vfs_list(const char* path, char* buf, size_t size);
int vfs_unlink(const char* path);
int vfs_mkdir(const char* path);
int vfs_rename(const char* oldp, const char* newp);
int vfs_symlink(const char* target, const char* linkpath);
int vfs_link(const char* oldpath, const char* newpath);

void vfs_init(void);

#endif
