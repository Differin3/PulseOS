#include "fs_cache.h"
#include "drivers/storage/ata.h"
#include "fs.h"
#include <string.h>

struct cache_slot {
    bool used;
    bool dirty;
    uint32_t lba;
    uint32_t stamp;
    uint8_t data[FS_SECTOR_SIZE];
};

static struct cache_slot slots[FS_CACHE_SLOTS];
static uint32_t clock;
static uint32_t hits;
static uint32_t misses;
static bool ready;

void fs_cache_init(void) {
    memset(slots, 0, sizeof(slots));
    clock = 1;
    hits = misses = 0;
    ready = true;
}

void fs_cache_invalidate_all(void) {
    for (int i = 0; i < FS_CACHE_SLOTS; i++) {
        slots[i].used = false;
        slots[i].dirty = false;
    }
}

static int slot_find(uint32_t lba) {
    for (int i = 0; i < FS_CACHE_SLOTS; i++)
        if (slots[i].used && slots[i].lba == lba) return i;
    return -1;
}

static int slot_evict(void) {
    int best = 0;
    uint32_t best_stamp = 0xFFFFFFFFu;
    for (int i = 0; i < FS_CACHE_SLOTS; i++) {
        if (!slots[i].used) return i;
        if (slots[i].stamp < best_stamp && !slots[i].dirty) {
            best_stamp = slots[i].stamp;
            best = i;
        }
    }
    /* Prefer clean; else oldest dirty */
    best = 0;
    best_stamp = slots[0].stamp;
    for (int i = 1; i < FS_CACHE_SLOTS; i++) {
        if (slots[i].stamp < best_stamp) {
            best_stamp = slots[i].stamp;
            best = i;
        }
    }
    if (slots[best].dirty) {
        if (disk_write_sector(slots[best].lba, slots[best].data) != 0) return -1;
        slots[best].dirty = false;
    }
    return best;
}

int fs_cache_sync(void) {
    if (!ready) return 0;
    for (int i = 0; i < FS_CACHE_SLOTS; i++) {
        if (slots[i].used && slots[i].dirty) {
            if (disk_write_sector(slots[i].lba, slots[i].data) != 0) return -1;
            slots[i].dirty = false;
        }
    }
    return 0;
}

int fs_cache_read_sector(uint32_t lba, void* buf) {
    if (!ready) return disk_read_sector(lba, buf);
    int s = slot_find(lba);
    if (s >= 0) {
        hits++;
        slots[s].stamp = ++clock;
        memcpy(buf, slots[s].data, FS_SECTOR_SIZE);
        return 0;
    }
    misses++;
    s = slot_evict();
    if (s < 0) return -1;
    if (disk_read_sector(lba, slots[s].data) != 0) return -1;
    slots[s].used = true;
    slots[s].dirty = false;
    slots[s].lba = lba;
    slots[s].stamp = ++clock;
    memcpy(buf, slots[s].data, FS_SECTOR_SIZE);
    return 0;
}

int fs_cache_write_sector(uint32_t lba, const void* buf) {
    if (!ready) return disk_write_sector(lba, (void*)buf);
    int s = slot_find(lba);
    if (s < 0) {
        s = slot_evict();
        if (s < 0) return -1;
        slots[s].used = true;
        slots[s].lba = lba;
    }
    memcpy(slots[s].data, buf, FS_SECTOR_SIZE);
    slots[s].dirty = true;
    slots[s].stamp = ++clock;
    return 0;
}

int fs_cache_read_sectors(uint32_t lba, uint32_t count, void* buf) {
    uint8_t* p = (uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++) {
        if (fs_cache_read_sector(lba + i, p + i * FS_SECTOR_SIZE) != 0) return -1;
    }
    return 0;
}

int fs_cache_write_sectors(uint32_t lba, uint32_t count, const void* buf) {
    const uint8_t* p = (const uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++) {
        if (fs_cache_write_sector(lba + i, p + i * FS_SECTOR_SIZE) != 0) return -1;
    }
    return 0;
}

uint32_t fs_cache_hits(void) { return hits; }
uint32_t fs_cache_misses(void) { return misses; }
