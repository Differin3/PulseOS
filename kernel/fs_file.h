#ifndef FS_FILE_H
#define FS_FILE_H

#include "fs.h"
#include <stdint.h>
#include <stddef.h>

#define FS_OPEN_MAX 64

void fs_ofile_release(int ofile);
int vfs_open(const char* path, int flags, uint16_t mode);
int vfs_openat(int dirfd, const char* path, int flags, uint16_t mode);
int vfs_close(int ofile);
int vfs_fread(int ofile, void* buf, size_t size);
int vfs_fwrite(int ofile, const void* data, size_t size);
int vfs_lseek(int ofile, int32_t offset, int whence);
int vfs_fstat(int ofile, struct fs_stat* st);
int vfs_dup(int tfd);
int vfs_dup2(int oldfd, int newfd);
int vfs_fcntl(int tfd, int cmd, int arg);
int vfs_fsync(int tfd);
int vfs_getdents(int tfd, char* buf, size_t size);

#define F_GETFL 1
#define F_SETFL 2

#endif
