#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* MOS filesystem — version 2 (attrs + journaled metadata) */
#define FS_MAGIC           0x4D4F5320u  /* "MOS " */
#define FS_VERSION         2u
#define FS_MAX_FILES       256
#define FS_FILENAME_LEN    255
#define FS_SECTOR_SIZE     512
#define FS_ROOT_PARENT     0xFFFFFFFFu
#define FS_SYMLINK_MAX     8
#define FS_JOURNAL_MAX_SEC 16

#define FS_FLAG_FREE       0x00
#define FS_FLAG_OCCUPIED   0x01
#define FS_FLAG_DIRECTORY  0x02
#define FS_FLAG_SYMLINK    0x04

#define FS_MODE_FILE       0644
#define FS_MODE_DIR        0755
#define FS_MODE_LINK       0777

#define O_RDONLY  0x0001
#define O_WRONLY  0x0002
#define O_RDWR    0x0003
#define O_CREAT   0x0100
#define O_TRUNC   0x0200
#define O_APPEND  0x0400

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct fs_file_entry {
    char name[FS_FILENAME_LEN];
    uint32_t start_sector;
    uint32_t sector_count;
    uint32_t size_bytes;
    uint8_t flags;
    uint32_t parent_dir;
    uint16_t mode;
    uint32_t mtime;
    uint16_t nlink;
} __attribute__((packed));

struct fs_boot_sector {
    uint32_t magic;
    uint32_t version;
    uint32_t total_sectors;
    uint32_t file_table_sector;
    uint32_t data_start_sector;
    uint32_t file_count;
    uint32_t free_bitmap_sector;
    uint32_t free_bitmap_size;
    uint32_t log_start_sector;
    uint32_t log_size;
    uint32_t log_next;          /* 0 = clean; else txn size in sectors (hdr+data) */
    uint32_t log_magic;
    uint32_t file_table_slots;  /* usually FS_MAX_FILES */
    uint8_t reserved[FS_SECTOR_SIZE - 52];
} __attribute__((packed));

struct fs_stat {
    uint32_t size;
    uint32_t mode;
    uint32_t mtime;
    uint32_t nlink;
    uint8_t flags;
    uint32_t start_sector;
    uint32_t sector_count;
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
int fs_symlink(const char* target, const char* linkpath);
int fs_readlink(const char* path, char* buf, size_t buf_size);

int fs_check_integrity(void);
int fs_recover(void);

/* Internal helpers used by VFS/fd layer */
int fs_lookup_index(const char* path, bool follow_symlinks);
int fs_index_stat(int idx, struct fs_stat* st);
int fs_index_read(int idx, uint32_t offset, void* buffer, size_t size);
int fs_index_write(int idx, uint32_t offset, const void* data, size_t size);
int fs_index_truncate(int idx, uint32_t new_size);
int fs_create_file(const char* path, uint16_t mode);
const struct fs_file_entry* fs_entry_at(int idx);
uint32_t fs_now(void);

/* Journal smoke: stage a dirty txn then recover (autotest) */
int fs_journal_selftest(void);

#endif
