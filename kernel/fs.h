#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* MOS filesystem — version 3 (inode + dirent + multi-extent) */
#define FS_MAGIC           0x4D4F5320u  /* "MOS " */
#define FS_VERSION         3u
#define FS_MAX_INODES      1024
#define FS_MAX_FILES       FS_MAX_INODES /* alias */
#define FS_FILENAME_LEN    255
#define FS_SECTOR_SIZE     512
#define FS_ROOT_INO        1u
#define FS_SYMLINK_MAX     8
#define FS_JOURNAL_MAX_SEC 32
#define FS_DIRECT_EXTENTS  4
#define FS_SYMLINK_INLINE  60

#define FS_FLAG_FREE       0x00
#define FS_FLAG_OCCUPIED   0x01
#define FS_FLAG_DIRECTORY  0x02
#define FS_FLAG_SYMLINK    0x04
#define FS_FLAG_FIFO       0x08
#define FS_FLAG_SOCK       0x10
#define FS_FLAG_BLK        0x20
#define FS_FLAG_CHR        0x40
#define FS_FLAG_COMPRESSED 0x80

#define FS_MODE_FILE       0644
#define FS_MODE_DIR        0755
#define FS_MODE_LINK       0777

#define O_RDONLY  0x0001
#define O_WRONLY  0x0002
#define O_RDWR    0x0003
#define O_CREAT   0x0100
#define O_TRUNC   0x0200
#define O_APPEND  0x0400
#define O_EXCL    0x0800
#define O_DIRECTORY 0x1000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct fs_extent {
    uint32_t lba;
    uint32_t count;
} __attribute__((packed));

struct fs_inode {
    uint16_t mode;
    uint16_t flags;
    uint16_t uid;
    uint16_t gid;
    uint16_t nlink;
    uint16_t pad0;
    uint32_t size_bytes;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint32_t parent;              /* for ".." */
    struct fs_extent direct[FS_DIRECT_EXTENTS];
    uint32_t indirect_lba;        /* 0 = none */
    uint32_t xattr_lba;           /* 0 = none */
    uint32_t rdev;
    char symlink_inline[FS_SYMLINK_INLINE];
} __attribute__((packed));

/* Compatibility alias used by older call sites */
typedef struct fs_inode fs_file_entry;

struct fs_boot_sector {
    uint32_t magic;
    uint32_t version;
    uint32_t total_sectors;
    uint32_t inode_table_sector;
    uint32_t data_start_sector;
    uint32_t inode_count;         /* allocated inodes (occupied count approx) */
    uint32_t free_bitmap_sector;
    uint32_t free_bitmap_size;
    uint32_t log_start_sector;
    uint32_t log_size;
    uint32_t log_next;
    uint32_t log_magic;
    uint32_t inode_slots;         /* FS_MAX_INODES */
    uint32_t root_ino;            /* FS_ROOT_INO */
    uint32_t quota_soft_blocks;   /* 0 = disabled */
    uint8_t reserved[FS_SECTOR_SIZE - 60];
} __attribute__((packed));

struct fs_stat {
    uint32_t size;
    uint32_t mode;
    uint32_t mtime;
    uint32_t atime;
    uint32_t ctime;
    uint32_t nlink;
    uint16_t uid;
    uint16_t gid;
    uint8_t flags;
    uint32_t start_sector;   /* first data lba if any */
    uint32_t sector_count;   /* total data sectors */
};

bool fs_ready(void);
int fs_init(int disk_id);

int fs_open(const char* filename, uint32_t* size);
int fs_read(const char* filename, void* buffer, size_t size);
int fs_write(const char* filename, const void* data, size_t size);
int fs_list_dir(const char* path, char* buffer, size_t buffer_size);
int fs_delete(const char* filename);
int fs_get_disk_usage(uint32_t* total_bytes, uint32_t* used_bytes, uint32_t* free_bytes);
int fs_resolve_path(const char* path, int* disk_id, char* filename);
int fs_create_dir(const char* path);
int fs_rm_rf(const char* path);

int fs_rename(const char* old_path, const char* new_path);
int fs_truncate(const char* path, uint32_t new_size);
int fs_stat(const char* path, struct fs_stat* st);
int fs_chmod(const char* path, uint16_t mode);
int fs_chown(const char* path, uint16_t uid, uint16_t gid);
int fs_symlink(const char* target, const char* linkpath);
int fs_readlink(const char* path, char* buf, size_t buf_size);
int fs_link(const char* oldpath, const char* newpath);
int fs_touch(const char* path);

int fs_check_integrity(void);
int fs_fsck(bool repair);
int fs_recover(void);
int fs_sync(void);

int fs_lookup_index(const char* path, bool follow_symlinks);
int fs_index_stat(int idx, struct fs_stat* st);
int fs_index_read(int idx, uint32_t offset, void* buffer, size_t size);
int fs_index_write(int idx, uint32_t offset, const void* data, size_t size);
int fs_index_truncate(int idx, uint32_t new_size);
int fs_create_file(const char* path, uint16_t mode);
const struct fs_inode* fs_entry_at(int idx);
uint32_t fs_now(void);

int fs_getxattr(const char* path, const char* name, void* buf, size_t size);
int fs_setxattr(const char* path, const char* name, const void* val, size_t size);

/* Streaming readdir: *off in/out cookie into directory blob */
int fs_readdir(const char* path, uint32_t* off, char* name_out, size_t name_cap, uint32_t* ino_out);

int fs_journal_selftest(void);
int fs_access_ok(int idx, int want_write);

uint16_t fs_current_uid(void);
void fs_set_current_uid(uint16_t uid);

#endif
