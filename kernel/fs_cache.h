#ifndef FS_CACHE_H
#define FS_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FS_CACHE_SLOTS 256

void fs_cache_init(void);
void fs_cache_invalidate_all(void);
int fs_cache_sync(void);

/* Cached disk I/O used by MOS */
int fs_cache_read_sector(uint32_t lba, void* buf);
int fs_cache_write_sector(uint32_t lba, const void* buf);
int fs_cache_read_sectors(uint32_t lba, uint32_t count, void* buf);
int fs_cache_write_sectors(uint32_t lba, uint32_t count, const void* buf);

uint32_t fs_cache_hits(void);
uint32_t fs_cache_misses(void);

#endif
