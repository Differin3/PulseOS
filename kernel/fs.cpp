#include "fs.h"
#include "drivers/storage/ata.h"
#include "drivers/storage/disk_manager.h"
#include "drivers/timer/pit.h"
#include "serial_log.h"
#include "heap.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define FS_TXN_MAGIC 0x4A524E31u /* JRN1 */

struct fs_txn_hdr {
    uint32_t magic;
    uint32_t nsec;
    uint32_t lba[FS_JOURNAL_MAX_SEC];
    uint32_t csum;
    uint8_t pad[FS_SECTOR_SIZE - 4 * (3 + FS_JOURNAL_MAX_SEC)];
} __attribute__((packed));

static struct fs_boot_sector boot_sector;
static struct fs_file_entry file_table[FS_MAX_FILES];
static bool fs_initialized = false;
static uint32_t root_dir_index = FS_ROOT_PARENT;
static uint8_t* bitmap = NULL;
static uint32_t bitmap_sectors = 0;

static uint32_t fs_csum32(const void* data, size_t len) {
    uint32_t sum = 0;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        sum += p[i];
        sum = (sum << 1) | (sum >> 31);
    }
    return sum;
}

uint32_t fs_now(void) {
    return timer_ms() / 1000u;
}

static uint32_t fs_table_sectors(void) {
    uint32_t slots = boot_sector.file_table_slots ? boot_sector.file_table_slots : FS_MAX_FILES;
    if (slots > FS_MAX_FILES) slots = FS_MAX_FILES;
    return (slots * (uint32_t)sizeof(struct fs_file_entry) + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
}

static int fs_load_bitmap(void) {
    if (bitmap) return 0;
    bitmap_sectors = (boot_sector.free_bitmap_size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    if (bitmap_sectors == 0)
        bitmap_sectors = (boot_sector.total_sectors + 7) / 8 / FS_SECTOR_SIZE + 1;
    bitmap = (uint8_t*)malloc(bitmap_sectors * FS_SECTOR_SIZE);
    if (!bitmap) return -1;
    memset(bitmap, 0, bitmap_sectors * FS_SECTOR_SIZE);
    return disk_read_sectors(boot_sector.free_bitmap_sector, bitmap_sectors, bitmap);
}

static int fs_save_bitmap_raw(void) {
    if (!bitmap) return 0;
    return disk_write_sectors(boot_sector.free_bitmap_sector, bitmap_sectors, bitmap);
}

static int fs_bitmap_test(uint32_t sector) {
    if (!bitmap || sector >= boot_sector.total_sectors) return 1;
    return (bitmap[sector / 8] & (1u << (sector % 8))) ? 1 : 0;
}

static void fs_bitmap_set(uint32_t sector, int occupied) {
    if (!bitmap || sector >= boot_sector.total_sectors) return;
    uint8_t bit = (uint8_t)(1u << (sector % 8));
    if (occupied) bitmap[sector / 8] |= bit;
    else bitmap[sector / 8] &= (uint8_t)~bit;
}

static uint32_t fs_alloc_base_sector(void) {
    uint32_t base = boot_sector.data_start_sector;
    if (boot_sector.log_size > 0 &&
        boot_sector.log_start_sector + boot_sector.log_size > base) {
        base = boot_sector.log_start_sector + boot_sector.log_size;
    }
    uint32_t table_end = boot_sector.file_table_sector + fs_table_sectors();
    if (table_end > base) base = table_end;
    return base;
}

static int fs_alloc_blocks(uint32_t count, uint32_t* start) {
    if (!bitmap || count == 0) return -1;
    uint32_t free_run = 0;
    uint32_t base = fs_alloc_base_sector();
    for (uint32_t i = base; i < boot_sector.total_sectors; i++) {
        if (!fs_bitmap_test(i)) {
            free_run++;
            if (free_run >= count) {
                *start = i - count + 1;
                return 0;
            }
        } else free_run = 0;
    }
    return -1;
}

static void fs_free_blocks(uint32_t start, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) fs_bitmap_set(start + i, 0);
}

/* ---- Journal: redo of up to FS_JOURNAL_MAX_SEC sectors ---- */

int fs_recover(void) {
    if (boot_sector.log_size == 0 || boot_sector.log_next == 0) return 0;
    uint8_t hdr_buf[FS_SECTOR_SIZE];
    if (disk_read_sector(boot_sector.log_start_sector, hdr_buf) != 0) return -1;
    struct fs_txn_hdr* hdr = (struct fs_txn_hdr*)hdr_buf;
    if (hdr->magic != FS_TXN_MAGIC || hdr->nsec == 0 || hdr->nsec > FS_JOURNAL_MAX_SEC) {
        boot_sector.log_next = 0;
        disk_write_sector(0, &boot_sector);
        return 0;
    }
    uint32_t expect = fs_csum32(&hdr->magic, sizeof(uint32_t) * (2 + hdr->nsec));
    if (expect != hdr->csum) {
        log_msg(LOG_ERR, "fs", "journal csum fail — discard");
        boot_sector.log_next = 0;
        disk_write_sector(0, &boot_sector);
        return 0;
    }
    uint8_t data[FS_SECTOR_SIZE];
    for (uint32_t i = 0; i < hdr->nsec; i++) {
        uint32_t log_sec = boot_sector.log_start_sector + 1 + i;
        if (disk_read_sector(log_sec, data) != 0) continue;
        disk_write_sector(hdr->lba[i], data);
    }
    boot_sector.log_next = 0;
    disk_write_sector(0, &boot_sector);
    log_msg(LOG_INFO, "fs", "journal recovered");
    return 0;
}

static int fs_journal_commit(const uint32_t* lbas, const uint8_t* const* sectors, uint32_t n) {
    if (n == 0 || n > FS_JOURNAL_MAX_SEC) return -1;
    if (boot_sector.log_size < n + 1) return -1;

    struct fs_txn_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = FS_TXN_MAGIC;
    hdr.nsec = n;
    for (uint32_t i = 0; i < n; i++) hdr.lba[i] = lbas[i];
    hdr.csum = fs_csum32(&hdr.magic, sizeof(uint32_t) * (2 + n));

    if (disk_write_sector(boot_sector.log_start_sector, &hdr) != 0) return -1;
    for (uint32_t i = 0; i < n; i++) {
        if (disk_write_sector(boot_sector.log_start_sector + 1 + i, (void*)sectors[i]) != 0)
            return -1;
    }
    boot_sector.log_next = n + 1;
    if (disk_write_sector(0, &boot_sector) != 0) return -1;

    for (uint32_t i = 0; i < n; i++) {
        if (disk_write_sector(lbas[i], (void*)sectors[i]) != 0) return -1;
    }
    boot_sector.log_next = 0;
    if (disk_write_sector(0, &boot_sector) != 0) return -1;
    return 0;
}

/* Persist file table + bitmap using journal for first N table sectors + first bitmap sector + SB is separate */
static int fs_persist_meta(void) {
    uint32_t tsec = fs_table_sectors();
    uint32_t n = 0;
    uint32_t lbas[FS_JOURNAL_MAX_SEC];
    const uint8_t* ptrs[FS_JOURNAL_MAX_SEC];
    static uint8_t chunk[FS_JOURNAL_MAX_SEC][FS_SECTOR_SIZE];

    uint32_t take = tsec;
    if (take > FS_JOURNAL_MAX_SEC - 2) take = FS_JOURNAL_MAX_SEC - 2;
    for (uint32_t i = 0; i < take; i++) {
        lbas[n] = boot_sector.file_table_sector + i;
        memcpy(chunk[n], ((uint8_t*)file_table) + i * FS_SECTOR_SIZE, FS_SECTOR_SIZE);
        ptrs[n] = chunk[n];
        n++;
    }
    if (bitmap && bitmap_sectors > 0 && n < FS_JOURNAL_MAX_SEC) {
        lbas[n] = boot_sector.free_bitmap_sector;
        memcpy(chunk[n], bitmap, FS_SECTOR_SIZE);
        ptrs[n] = chunk[n];
        n++;
    }
    if (fs_journal_commit(lbas, ptrs, n) != 0) return -1;

    /* Remaining table/bitmap sectors (unjournaled but after txn marker cleared) */
    if (tsec > take) {
        if (disk_write_sectors(boot_sector.file_table_sector + take, tsec - take,
                               ((uint8_t*)file_table) + take * FS_SECTOR_SIZE) != 0)
            return -1;
    }
    if (bitmap && bitmap_sectors > 1) {
        if (disk_write_sectors(boot_sector.free_bitmap_sector + 1, bitmap_sectors - 1,
                               bitmap + FS_SECTOR_SIZE) != 0)
            return -1;
    }
    if (disk_write_sector(0, &boot_sector) != 0) return -1;
    return 0;
}

static int fs_save_file_table(void) {
    return fs_persist_meta();
}

int fs_journal_selftest(void) {
    if (!fs_initialized || boot_sector.log_size < 2) return -1;
    uint8_t scratch[FS_SECTOR_SIZE];
    memset(scratch, 0xA5, sizeof(scratch));
    uint32_t lba = boot_sector.log_start_sector; /* reuse area carefully: write to a data free sector */
    uint32_t start = 0;
    if (fs_alloc_blocks(1, &start) != 0) return -1;
    lba = start;
    const uint8_t* p = scratch;
    if (fs_journal_commit(&lba, &p, 1) != 0) return -1;
    /* Simulate crash: set dirty and recover */
    boot_sector.log_next = 2;
    disk_write_sector(0, &boot_sector);
    fs_recover();
    uint8_t verify[FS_SECTOR_SIZE];
    if (disk_read_sector(lba, verify) != 0) return -1;
    for (int i = 0; i < FS_SECTOR_SIZE; i++) if (verify[i] != 0xA5) return -1;
    fs_free_blocks(lba, 1);
    fs_persist_meta();
    return 0;
}

int fs_check_integrity(void) {
    if (!fs_initialized) return -1;
    int errors = 0;
    bool seen[FS_MAX_FILES];
    memset(seen, 0, sizeof(seen));

    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!(file_table[i].flags & FS_FLAG_OCCUPIED)) continue;
        uint32_t parent = file_table[i].parent_dir;
        if (parent != FS_ROOT_PARENT) {
            if (parent >= FS_MAX_FILES || !(file_table[parent].flags & FS_FLAG_DIRECTORY))
                errors++;
        }
        uint32_t start = file_table[i].start_sector;
        uint32_t count = file_table[i].sector_count;
        if (count == 0) continue;
        if (start >= boot_sector.total_sectors || start + count > boot_sector.total_sectors) {
            errors++;
            continue;
        }
        for (uint32_t j = 0; j < count; j++) {
            if (!fs_bitmap_test(start + j)) {
                errors++;
                fs_bitmap_set(start + j, 1);
            }
        }
        /* overlap vs later files */
        for (int k = i + 1; k < FS_MAX_FILES; k++) {
            if (!(file_table[k].flags & FS_FLAG_OCCUPIED) || file_table[k].sector_count == 0)
                continue;
            uint32_t s2 = file_table[k].start_sector;
            uint32_t c2 = file_table[k].sector_count;
            if (!(start + count <= s2 || s2 + c2 <= start)) errors++;
        }
    }
    if (errors > 0) fs_save_bitmap_raw();
    return errors;
}

static int fs_find_free_slot(void) {
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (file_table[i].flags == FS_FLAG_FREE) return i;
    return -1;
}

static int fs_find_file_in_dir(const char* name, uint32_t parent_idx) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if ((file_table[i].flags & FS_FLAG_OCCUPIED) && file_table[i].parent_dir == parent_idx) {
            if (strcmp(file_table[i].name, name) == 0) return i;
        }
    }
    return -1;
}

static int fs_resolve_component(const char* path, bool follow, int depth);

static int fs_follow_if_link(int idx, bool follow, int depth) {
    if (idx < 0) return idx;
    if (!follow || !(file_table[idx].flags & FS_FLAG_SYMLINK)) return idx;
    if (depth >= FS_SYMLINK_MAX) return -1;
    char target[FS_FILENAME_LEN];
    if (file_table[idx].size_bytes == 0 || file_table[idx].size_bytes >= sizeof(target))
        return -1;
    if (fs_index_read(idx, 0, target, file_table[idx].size_bytes) < 0) return -1;
    target[file_table[idx].size_bytes] = 0;
    return fs_resolve_component(target, true, depth + 1);
}

/* Resolve absolute or relative-to-root path to index */
static int fs_resolve_component(const char* path, bool follow, int depth) {
    if (!path || !path[0]) return -1;
    if (path[0] == '/' && path[1] == 0) return -2; /* root sentinel */

    char tmp[256];
    size_t n = 0;
    while (path[n] && n + 1 < sizeof(tmp)) {
        tmp[n] = path[n];
        n++;
    }
    tmp[n] = 0;

    uint32_t parent = root_dir_index;
    const char* p = tmp;
    if (*p == '/') p++;

    char name[FS_FILENAME_LEN];
    int last = -1;
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < FS_FILENAME_LEN - 1) name[i++] = *p++;
        name[i] = 0;
        if (*p == '/') p++;
        if (name[0] == 0) continue;
        if (name[0] == '.' && name[1] == 0) continue;
        if (name[0] == '.' && name[1] == '.' && name[2] == 0) {
            if (parent == root_dir_index) continue;
            parent = file_table[parent].parent_dir;
            last = (parent == root_dir_index) ? -2 : (int)parent;
            continue;
        }
        int idx = fs_find_file_in_dir(name, parent);
        if (idx < 0) return -1;
        idx = fs_follow_if_link(idx, follow && (*p != 0 || follow), depth);
        if (idx < 0) return -1;
        if (*p) {
            if (!(file_table[idx].flags & FS_FLAG_DIRECTORY)) return -1;
            parent = (uint32_t)idx;
        }
        last = idx;
    }
    return last;
}

int fs_lookup_index(const char* path, bool follow_symlinks) {
    if (!fs_initialized || !path) return -1;
    if (path[0] == '/' && path[1] == 0) return -2;
    int idx = fs_resolve_component(path, follow_symlinks, 0);
    if (idx == -2) return -2;
    return idx;
}

static bool fs_layout_valid(void) {
    if (boot_sector.magic != FS_MAGIC) return false;
    if (boot_sector.version != FS_VERSION) return false;
    if (boot_sector.file_table_sector == 0 || boot_sector.free_bitmap_sector == 0) return false;
    if (boot_sector.total_sectors < 64) return false;
    if (boot_sector.file_table_slots == 0 || boot_sector.file_table_slots > FS_MAX_FILES)
        return false;
    uint32_t table_end = boot_sector.file_table_sector + fs_table_sectors();
    if (table_end >= boot_sector.total_sectors) return false;
    if (boot_sector.free_bitmap_sector < table_end) return false;
    uint32_t bitmap_bytes = boot_sector.free_bitmap_size;
    if (bitmap_bytes == 0) return false;
    uint32_t bitmap_secs = (bitmap_bytes + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    uint32_t bitmap_end = boot_sector.free_bitmap_sector + bitmap_secs;
    if (bitmap_end > boot_sector.total_sectors) return false;
    if (boot_sector.data_start_sector < bitmap_end) return false;
    if (boot_sector.log_size > 0) {
        if (boot_sector.log_start_sector == 0) return false;
        if (boot_sector.log_start_sector + boot_sector.log_size > boot_sector.total_sectors)
            return false;
    }
    return true;
}

bool fs_ready(void) { return fs_initialized; }

const struct fs_file_entry* fs_entry_at(int idx) {
    if (idx < 0 || idx >= FS_MAX_FILES) return 0;
    if (!(file_table[idx].flags & FS_FLAG_OCCUPIED)) return 0;
    return &file_table[idx];
}

int fs_index_stat(int idx, struct fs_stat* st) {
    if (!st || idx < 0 || idx >= FS_MAX_FILES) return -1;
    if (!(file_table[idx].flags & FS_FLAG_OCCUPIED)) return -1;
    st->size = file_table[idx].size_bytes;
    st->mode = file_table[idx].mode;
    st->mtime = file_table[idx].mtime;
    st->nlink = file_table[idx].nlink;
    st->flags = file_table[idx].flags;
    st->start_sector = file_table[idx].start_sector;
    st->sector_count = file_table[idx].sector_count;
    return 0;
}

int fs_index_read(int idx, uint32_t offset, void* buffer, size_t size) {
    if (idx < 0 || !buffer) return -1;
    if (!(file_table[idx].flags & FS_FLAG_OCCUPIED)) return -1;
    if (file_table[idx].flags & FS_FLAG_DIRECTORY) return -1;
    if (offset >= file_table[idx].size_bytes) return 0;
    size_t avail = file_table[idx].size_bytes - offset;
    if (size > avail) size = avail;
    if (size == 0) return 0;
    uint32_t start_off = offset;
    uint32_t end_off = offset + (uint32_t)size;
    uint32_t first_sec = start_off / FS_SECTOR_SIZE;
    uint32_t last_sec = (end_off + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    uint32_t nsec = last_sec - first_sec;
    uint8_t* tmp = (uint8_t*)malloc(nsec * FS_SECTOR_SIZE);
    if (!tmp) return -1;
    if (disk_read_sectors(file_table[idx].start_sector + first_sec, nsec, tmp) != 0) {
        free(tmp);
        return -1;
    }
    memcpy(buffer, tmp + (start_off % FS_SECTOR_SIZE), size);
    free(tmp);
    return (int)size;
}

int fs_index_truncate(int idx, uint32_t new_size) {
    if (idx < 0) return -1;
    if (!(file_table[idx].flags & FS_FLAG_OCCUPIED)) return -1;
    if (file_table[idx].flags & FS_FLAG_DIRECTORY) return -1;
    if (!bitmap && fs_load_bitmap() != 0) return -1;

    uint32_t need = new_size ? (new_size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE : 0;
    uint32_t old = file_table[idx].sector_count;
    uint32_t old_start = file_table[idx].start_sector;

    if (need == old) {
        file_table[idx].size_bytes = new_size;
        file_table[idx].mtime = fs_now();
        return fs_persist_meta();
    }

    if (need == 0) {
        if (old > 0) fs_free_blocks(old_start, old);
        file_table[idx].start_sector = 0;
        file_table[idx].sector_count = 0;
        file_table[idx].size_bytes = 0;
        file_table[idx].mtime = fs_now();
        return fs_persist_meta();
    }

    uint32_t ns = 0;
    if (fs_alloc_blocks(need, &ns) != 0) return -1;
    uint8_t* buf = (uint8_t*)malloc(need * FS_SECTOR_SIZE);
    if (!buf) {
        fs_free_blocks(ns, need);
        return -1;
    }
    memset(buf, 0, need * FS_SECTOR_SIZE);
    if (old > 0) {
        uint32_t copy = old < need ? old : need;
        if (disk_read_sectors(old_start, copy, buf) != 0) {
            free(buf);
            fs_free_blocks(ns, need);
            return -1;
        }
    }
    if (disk_write_sectors(ns, need, buf) != 0) {
        free(buf);
        fs_free_blocks(ns, need);
        return -1;
    }
    free(buf);
    if (old > 0) fs_free_blocks(old_start, old);
    for (uint32_t i = 0; i < need; i++) fs_bitmap_set(ns + i, 1);
    file_table[idx].start_sector = ns;
    file_table[idx].sector_count = need;
    file_table[idx].size_bytes = new_size;
    file_table[idx].mtime = fs_now();
    return fs_persist_meta();
}

int fs_index_write(int idx, uint32_t offset, const void* data, size_t size) {
    if (idx < 0 || (!data && size)) return -1;
    if (!(file_table[idx].flags & FS_FLAG_OCCUPIED)) return -1;
    if (file_table[idx].flags & (FS_FLAG_DIRECTORY)) return -1;
    uint32_t end = offset + (uint32_t)size;
    if (end < offset) return -1;
    if (end > file_table[idx].size_bytes) {
        if (fs_index_truncate(idx, end) != 0) return -1;
    }
    if (size == 0) return 0;
    uint32_t first = offset / FS_SECTOR_SIZE;
    uint32_t last = (offset + (uint32_t)size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    uint32_t nsec = last - first;
    uint8_t* tmp = (uint8_t*)malloc(nsec * FS_SECTOR_SIZE);
    if (!tmp) return -1;
    if (disk_read_sectors(file_table[idx].start_sector + first, nsec, tmp) != 0) {
        free(tmp);
        return -1;
    }
    memcpy(tmp + (offset % FS_SECTOR_SIZE), data, size);
    if (disk_write_sectors(file_table[idx].start_sector + first, nsec, tmp) != 0) {
        free(tmp);
        return -1;
    }
    free(tmp);
    file_table[idx].mtime = fs_now();
    if (fs_persist_meta() != 0) return -1;
    return (int)size;
}

static int fs_parent_and_name(const char* path, uint32_t* parent_out, char* name_out) {
    if (!path || path[0] != '/') return -1;
    const char* slash = strrchr(path, '/');
    if (!slash) return -1;
    const char* base = slash + 1;
    if (!base[0]) return -1;
    strncpy(name_out, base, FS_FILENAME_LEN - 1);
    name_out[FS_FILENAME_LEN - 1] = 0;
    if (slash == path) {
        *parent_out = root_dir_index;
        return 0;
    }
    char dir[256];
    size_t len = (size_t)(slash - path);
    if (len >= sizeof(dir)) return -1;
    memcpy(dir, path, len);
    dir[len] = 0;
    int didx = fs_lookup_index(dir, true);
    if (didx < 0 || !(file_table[didx].flags & FS_FLAG_DIRECTORY)) return -1;
    *parent_out = (uint32_t)didx;
    return 0;
}

int fs_create_file(const char* path, uint16_t mode) {
    if (!fs_initialized) return -1;
    if (fs_lookup_index(path, false) >= 0) return -1;
    uint32_t parent;
    char name[FS_FILENAME_LEN];
    if (fs_parent_and_name(path, &parent, name) != 0) return -1;
    int idx = fs_find_free_slot();
    if (idx < 0) return -1;
    memset(&file_table[idx], 0, sizeof(file_table[idx]));
    strncpy(file_table[idx].name, name, FS_FILENAME_LEN - 1);
    file_table[idx].flags = FS_FLAG_OCCUPIED;
    file_table[idx].parent_dir = parent;
    file_table[idx].mode = mode ? mode : FS_MODE_FILE;
    file_table[idx].mtime = fs_now();
    file_table[idx].nlink = 1;
    boot_sector.file_count++;
    if (fs_persist_meta() != 0) return -1;
    return idx;
}

int fs_init(int disk_id) {
    if (disk_id >= 0 && disk_select(disk_id) != 0) return -1;
    if (fs_initialized) return 0;

    log_msg(LOG_INFO, "fs", "init read superblock");
    if (disk_read_sector(0, &boot_sector) != 0) {
        log_msg(LOG_ERR, "fs", "superblock read fail");
        return -1;
    }

    bool need_create = !fs_layout_valid();
    if (need_create && boot_sector.magic == FS_MAGIC)
        log_msg(LOG_ERR, "fs", "old/invalid MOS layout — recreating");

    if (need_create) {
        log_msg(LOG_INFO, "fs", "create new filesystem v2");
        memset(&boot_sector, 0, sizeof(boot_sector));
        boot_sector.magic = FS_MAGIC;
        boot_sector.version = FS_VERSION;
        boot_sector.total_sectors = disk_get_size_sectors();
        if (boot_sector.total_sectors == 0) boot_sector.total_sectors = 1024;
        boot_sector.file_table_sector = 1;
        boot_sector.file_table_slots = FS_MAX_FILES;
        uint32_t table_sectors = fs_table_sectors();
        boot_sector.file_count = 0;
        boot_sector.free_bitmap_sector = 1 + table_sectors;
        boot_sector.free_bitmap_size = (boot_sector.total_sectors + 7) / 8;
        uint32_t bitmap_secs =
            (boot_sector.free_bitmap_size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
        boot_sector.data_start_sector = boot_sector.free_bitmap_sector + bitmap_secs;
        uint32_t log_sectors = 32;
        boot_sector.log_start_sector = boot_sector.data_start_sector;
        boot_sector.log_size = log_sectors;
        boot_sector.log_next = 0;
        boot_sector.log_magic = 0x4C4F4700;

        memset(file_table, 0, sizeof(file_table));
        for (int i = 0; i < FS_MAX_FILES; i++) {
            file_table[i].flags = FS_FLAG_FREE;
            file_table[i].parent_dir = FS_ROOT_PARENT;
        }
        root_dir_index = FS_ROOT_PARENT;

        bitmap_sectors = bitmap_secs;
        bitmap = (uint8_t*)malloc(bitmap_sectors * FS_SECTOR_SIZE);
        if (!bitmap) return -1;
        memset(bitmap, 0, bitmap_sectors * FS_SECTOR_SIZE);
        for (uint32_t i = 0; i < boot_sector.data_start_sector + log_sectors; i++)
            fs_bitmap_set(i, 1);
        if (disk_write_sectors(boot_sector.free_bitmap_sector, bitmap_sectors, bitmap) != 0)
            return -1;
        if (disk_write_sector(0, &boot_sector) != 0) return -1;
        if (disk_write_sectors(boot_sector.file_table_sector, table_sectors, file_table) != 0)
            return -1;

        fs_initialized = true;
        fs_create_dir("/bin");
        fs_create_dir("/usr");
        fs_create_dir("/usr/bin");
        fs_create_dir("/etc");
        fs_create_dir("/etc/network");
        fs_create_dir("/dev");
        fs_create_dir("/mnt");
        fs_create_dir("/home");
        fs_create_dir("/tmp");
        fs_create_dir("/www");
        fs_create_dir("/var");
        log_msg(LOG_INFO, "fs", "init ok (new v2)");
        return 0;
    }

    log_msg(LOG_INFO, "fs", "load existing filesystem");
    if (disk_read_sectors(boot_sector.file_table_sector, fs_table_sectors(), file_table) != 0)
        return -1;
    fs_initialized = true;
    if (fs_load_bitmap() != 0) return -1;
    if (boot_sector.log_next > 0) fs_recover();
    (void)fs_check_integrity();
    root_dir_index = FS_ROOT_PARENT;
    log_msg(LOG_INFO, "fs", "init ok");
    return 0;
}

int fs_open(const char* filename, uint32_t* size) {
    int idx = fs_lookup_index(filename, true);
    if (idx < 0) return -1;
    if (file_table[idx].flags & FS_FLAG_DIRECTORY) return -1;
    if (size) *size = file_table[idx].size_bytes;
    return 0;
}

int fs_read(const char* filename, void* buffer, size_t size) {
    int idx = fs_lookup_index(filename, true);
    if (idx < 0) return -1;
    return fs_index_read(idx, 0, buffer, size);
}

int fs_write(const char* filename, const void* data, size_t size) {
    if (!fs_initialized) return -1;
    if (!bitmap && fs_load_bitmap() != 0) return -1;
    int idx = fs_lookup_index(filename, false);
    if (idx < 0) {
        idx = fs_create_file(filename, FS_MODE_FILE);
        if (idx < 0) return -1;
    }
    if (file_table[idx].flags & FS_FLAG_DIRECTORY) return -1;
    if (fs_index_truncate(idx, (uint32_t)size) != 0) return -1;
    if (size == 0) return 0;
    return fs_index_write(idx, 0, data, size) == (int)size ? 0 : -1;
}

int fs_truncate(const char* path, uint32_t new_size) {
    int idx = fs_lookup_index(path, true);
    if (idx < 0) return -1;
    return fs_index_truncate(idx, new_size);
}

int fs_list_dir(const char* path, char* buffer, size_t buffer_size) {
    if (!fs_initialized || !buffer || buffer_size == 0) return -1;
    uint32_t parent = root_dir_index;
    if (path && !(path[0] == '/' && path[1] == 0) && path[0]) {
        int dir_idx = fs_lookup_index(path, true);
        if (dir_idx < 0 || !(file_table[dir_idx].flags & FS_FLAG_DIRECTORY)) return -1;
        parent = (uint32_t)dir_idx;
    }
    size_t pos = 0;
    for (int i = 0; i < FS_MAX_FILES && pos + 1 < buffer_size; i++) {
        if ((file_table[i].flags & FS_FLAG_OCCUPIED) && file_table[i].parent_dir == parent) {
            const char* name = file_table[i].name;
            while (*name && pos + 1 < buffer_size) buffer[pos++] = *name++;
            if (file_table[i].flags & FS_FLAG_DIRECTORY) {
                if (pos + 1 < buffer_size) buffer[pos++] = '/';
            } else if (file_table[i].flags & FS_FLAG_SYMLINK) {
                if (pos + 1 < buffer_size) buffer[pos++] = '@';
            }
            if (pos + 1 < buffer_size) buffer[pos++] = '\n';
        }
    }
    buffer[pos] = 0;
    return (int)pos;
}

int fs_delete(const char* filename) {
    if (!fs_initialized) return -1;
    int idx = fs_lookup_index(filename, false);
    if (idx < 0) return -1;
    if (file_table[idx].flags & FS_FLAG_DIRECTORY) {
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if ((file_table[i].flags & FS_FLAG_OCCUPIED) &&
                file_table[i].parent_dir == (uint32_t)idx)
                return -1;
        }
    }
    if (file_table[idx].sector_count > 0)
        fs_free_blocks(file_table[idx].start_sector, file_table[idx].sector_count);
    memset(&file_table[idx], 0, sizeof(file_table[idx]));
    file_table[idx].flags = FS_FLAG_FREE;
    file_table[idx].parent_dir = FS_ROOT_PARENT;
    if (boot_sector.file_count) boot_sector.file_count--;
    return fs_persist_meta();
}

int fs_rename(const char* old_path, const char* new_path) {
    if (!fs_initialized) return -1;
    int idx = fs_lookup_index(old_path, false);
    if (idx < 0) return -1;
    if (fs_lookup_index(new_path, false) >= 0) return -1;
    uint32_t parent;
    char name[FS_FILENAME_LEN];
    if (fs_parent_and_name(new_path, &parent, name) != 0) return -1;
    strncpy(file_table[idx].name, name, FS_FILENAME_LEN - 1);
    file_table[idx].name[FS_FILENAME_LEN - 1] = 0;
    file_table[idx].parent_dir = parent;
    file_table[idx].mtime = fs_now();
    return fs_persist_meta();
}

int fs_stat(const char* path, struct fs_stat* st) {
    int idx = fs_lookup_index(path, false);
    if (idx < 0) return -1;
    return fs_index_stat(idx, st);
}

int fs_chmod(const char* path, uint16_t mode) {
    int idx = fs_lookup_index(path, false);
    if (idx < 0) return -1;
    file_table[idx].mode = mode;
    file_table[idx].mtime = fs_now();
    return fs_persist_meta();
}

int fs_symlink(const char* target, const char* linkpath) {
    if (!target || !linkpath) return -1;
    if (fs_lookup_index(linkpath, false) >= 0) return -1;
    int idx = fs_create_file(linkpath, FS_MODE_LINK);
    if (idx < 0) return -1;
    file_table[idx].flags = FS_FLAG_OCCUPIED | FS_FLAG_SYMLINK;
    size_t len = 0;
    while (target[len]) len++;
    if (fs_index_truncate(idx, (uint32_t)len) != 0) return -1;
    if (len && fs_index_write(idx, 0, target, len) != (int)len) return -1;
    return 0;
}

int fs_readlink(const char* path, char* buf, size_t buf_size) {
    int idx = fs_lookup_index(path, false);
    if (idx < 0 || !(file_table[idx].flags & FS_FLAG_SYMLINK)) return -1;
    if (!buf || buf_size == 0) return -1;
    size_t n = file_table[idx].size_bytes;
    if (n + 1 > buf_size) n = buf_size - 1;
    if (fs_index_read(idx, 0, buf, n) < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

int fs_get_disk_usage(uint32_t* total_bytes, uint32_t* used_bytes, uint32_t* free_bytes) {
    if (!fs_initialized) return -1;
    uint32_t total = boot_sector.total_sectors * FS_SECTOR_SIZE;
    uint32_t used_sec = 0;
    for (uint32_t i = 0; i < boot_sector.total_sectors; i++)
        if (fs_bitmap_test(i)) used_sec++;
    uint32_t used = used_sec * FS_SECTOR_SIZE;
    if (total_bytes) *total_bytes = total;
    if (used_bytes) *used_bytes = used;
    if (free_bytes) *free_bytes = total > used ? total - used : 0;
    return 0;
}

int fs_resolve_path(const char* path, int* disk_id, char* filename) {
    if (disk_id) *disk_id = 0;
    if (!filename) return -1;
    const char* p = path;
    if (*p == '/') p++;
    int i = 0;
    while (*p && *p != '/' && i < FS_FILENAME_LEN - 1) filename[i++] = *p++;
    filename[i] = 0;
    return 0;
}

int fs_create_dir(const char* path) {
    if (!fs_initialized || !path) return -1;
    char copy[256];
    size_t n = 0;
    while (path[n] && n + 1 < sizeof(copy)) {
        copy[n] = path[n];
        n++;
    }
    copy[n] = 0;

    uint32_t parent = root_dir_index;
    char* p = copy;
    if (*p == '/') p++;
    while (*p) {
        char* slash = p;
        while (*slash && *slash != '/') slash++;
        char save = *slash;
        *slash = 0;
        if (p[0]) {
            int idx = fs_find_file_in_dir(p, parent);
            if (idx < 0) {
                idx = fs_find_free_slot();
                if (idx < 0) return -1;
                memset(&file_table[idx], 0, sizeof(file_table[idx]));
                strncpy(file_table[idx].name, p, FS_FILENAME_LEN - 1);
                file_table[idx].flags = FS_FLAG_OCCUPIED | FS_FLAG_DIRECTORY;
                file_table[idx].parent_dir = parent;
                file_table[idx].mode = FS_MODE_DIR;
                file_table[idx].mtime = fs_now();
                file_table[idx].nlink = 1;
                boot_sector.file_count++;
                if (fs_persist_meta() != 0) return -1;
                parent = (uint32_t)idx;
            } else {
                if (!(file_table[idx].flags & FS_FLAG_DIRECTORY)) return -1;
                parent = (uint32_t)idx;
            }
        }
        *slash = save;
        if (!save) break;
        p = slash + 1;
    }
    return 0;
}

int fs_rm_rf(const char* path) {
    if (!path || (path[0] == '/' && path[1] == 0)) return -1;
    struct fs_stat st;
    if (fs_stat(path, &st) != 0) return -1;
    if (st.flags & FS_FLAG_DIRECTORY) {
        char listing[1024];
        if (fs_list_dir(path, listing, sizeof(listing)) < 0) return -1;
        const char* p = listing;
        while (*p) {
            char name[FS_FILENAME_LEN];
            int n = 0;
            while (*p && *p != '\n' && n < FS_FILENAME_LEN - 1) {
                if (*p != '/' && *p != '@') name[n++] = *p;
                p++;
            }
            name[n] = 0;
            if (*p == '\n') p++;
            if (!name[0]) continue;
            char child[256];
            size_t c = 0;
            while (path[c] && c + 1 < sizeof(child)) {
                child[c] = path[c];
                c++;
            }
            if (c > 0 && child[c - 1] != '/' && c + 1 < sizeof(child)) child[c++] = '/';
            for (int k = 0; name[k] && c + 1 < sizeof(child); k++) child[c++] = name[k];
            child[c] = 0;
            if (fs_rm_rf(child) != 0) return -1;
        }
    }
    return fs_delete(path);
}
