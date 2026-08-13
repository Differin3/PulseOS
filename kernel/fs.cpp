#include "fs.h"
#include "fs_cache.h"
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
#define FS_INDIRECT_MAX ((FS_SECTOR_SIZE) / (uint32_t)sizeof(struct fs_extent))
#define FS_DIRENT_HDR 5

struct fs_txn_hdr {
    uint32_t magic;
    uint32_t nsec;
    uint32_t lba[FS_JOURNAL_MAX_SEC];
    uint32_t csum;
    uint8_t pad[FS_SECTOR_SIZE - 4 * (3 + FS_JOURNAL_MAX_SEC)];
} __attribute__((packed));

static struct fs_boot_sector boot_sector;
static struct fs_inode inodes[FS_MAX_INODES];
static bool fs_initialized = false;
static uint8_t* bitmap = NULL;
static uint32_t bitmap_sectors = 0;
static uint16_t g_uid = 0;
static uint32_t blocks_used_uid0 = 0;

static int d_read(uint32_t lba, void* buf) { return fs_cache_read_sector(lba, buf); }
static int d_write(uint32_t lba, const void* buf) { return fs_cache_write_sector(lba, buf); }
static int d_read_n(uint32_t lba, uint32_t n, void* buf) { return fs_cache_read_sectors(lba, n, buf); }
static int d_write_n(uint32_t lba, uint32_t n, const void* buf) { return fs_cache_write_sectors(lba, n, buf); }

static uint32_t fs_csum32(const void* data, size_t len) {
    uint32_t sum = 0;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        sum += p[i];
        sum = (sum << 1) | (sum >> 31);
    }
    return sum;
}

uint32_t fs_now(void) { return timer_ms() / 1000u; }
uint16_t fs_current_uid(void) { return g_uid; }
void fs_set_current_uid(uint16_t uid) { g_uid = uid; }

static uint32_t fs_inode_sectors(void) {
    uint32_t slots = boot_sector.inode_slots ? boot_sector.inode_slots : FS_MAX_INODES;
    if (slots > FS_MAX_INODES) slots = FS_MAX_INODES;
    return (slots * (uint32_t)sizeof(struct fs_inode) + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
}

static int fs_load_bitmap(void) {
    if (bitmap) return 0;
    bitmap_sectors = (boot_sector.free_bitmap_size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    if (bitmap_sectors == 0) return -1;
    bitmap = (uint8_t*)malloc(bitmap_sectors * FS_SECTOR_SIZE);
    if (!bitmap) return -1;
    memset(bitmap, 0, bitmap_sectors * FS_SECTOR_SIZE);
    return d_read_n(boot_sector.free_bitmap_sector, bitmap_sectors, bitmap);
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
        boot_sector.log_start_sector + boot_sector.log_size > base)
        base = boot_sector.log_start_sector + boot_sector.log_size;
    uint32_t table_end = boot_sector.inode_table_sector + fs_inode_sectors();
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
                for (uint32_t j = 0; j < count; j++) fs_bitmap_set(*start + j, 1);
                blocks_used_uid0 += count;
                return 0;
            }
        } else free_run = 0;
    }
    return -1;
}

static void fs_free_blocks(uint32_t start, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (fs_bitmap_test(start + i)) {
            fs_bitmap_set(start + i, 0);
            if (blocks_used_uid0) blocks_used_uid0--;
        }
    }
}

/* ---- Journal ---- */

int fs_recover(void) {
    if (boot_sector.log_size == 0 || boot_sector.log_next == 0) return 0;
    uint8_t hdr_buf[FS_SECTOR_SIZE];
    if (d_read(boot_sector.log_start_sector, hdr_buf) != 0) return -1;
    struct fs_txn_hdr* hdr = (struct fs_txn_hdr*)hdr_buf;
    if (hdr->magic != FS_TXN_MAGIC || hdr->nsec == 0 || hdr->nsec > FS_JOURNAL_MAX_SEC) {
        boot_sector.log_next = 0;
        d_write(0, &boot_sector);
        return 0;
    }
    uint32_t expect = fs_csum32(&hdr->magic, sizeof(uint32_t) * (2 + hdr->nsec));
    if (expect != hdr->csum) {
        log_msg(LOG_ERR, "fs", "journal csum fail — discard");
        boot_sector.log_next = 0;
        d_write(0, &boot_sector);
        return 0;
    }
    uint8_t data[FS_SECTOR_SIZE];
    for (uint32_t i = 0; i < hdr->nsec; i++) {
        if (d_read(boot_sector.log_start_sector + 1 + i, data) != 0) continue;
        d_write(hdr->lba[i], data);
    }
    boot_sector.log_next = 0;
    d_write(0, &boot_sector);
    fs_cache_sync();
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
    if (d_write(boot_sector.log_start_sector, &hdr) != 0) return -1;
    for (uint32_t i = 0; i < n; i++)
        if (d_write(boot_sector.log_start_sector + 1 + i, sectors[i]) != 0) return -1;
    boot_sector.log_next = n + 1;
    if (d_write(0, &boot_sector) != 0) return -1;
    for (uint32_t i = 0; i < n; i++)
        if (d_write(lbas[i], sectors[i]) != 0) return -1;
    boot_sector.log_next = 0;
    if (d_write(0, &boot_sector) != 0) return -1;
    return fs_cache_sync();
}

static int fs_persist_meta(void) {
    uint32_t tsec = fs_inode_sectors();
    uint32_t n = 0;
    uint32_t lbas[FS_JOURNAL_MAX_SEC];
    const uint8_t* ptrs[FS_JOURNAL_MAX_SEC];
    static uint8_t chunk[FS_JOURNAL_MAX_SEC][FS_SECTOR_SIZE];

    uint32_t take = tsec;
    if (take > FS_JOURNAL_MAX_SEC - 2) take = FS_JOURNAL_MAX_SEC - 2;
    for (uint32_t i = 0; i < take; i++) {
        lbas[n] = boot_sector.inode_table_sector + i;
        memcpy(chunk[n], ((uint8_t*)inodes) + i * FS_SECTOR_SIZE, FS_SECTOR_SIZE);
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
    if (tsec > take) {
        if (d_write_n(boot_sector.inode_table_sector + take, tsec - take,
                      ((uint8_t*)inodes) + take * FS_SECTOR_SIZE) != 0)
            return -1;
    }
    if (bitmap && bitmap_sectors > 1) {
        if (d_write_n(boot_sector.free_bitmap_sector + 1, bitmap_sectors - 1,
                      bitmap + FS_SECTOR_SIZE) != 0)
            return -1;
    }
    if (d_write(0, &boot_sector) != 0) return -1;
    return fs_cache_sync();
}

int fs_sync(void) { return fs_persist_meta(); }

int fs_journal_selftest(void) {
    if (!fs_initialized || boot_sector.log_size < 2) return -1;
    uint8_t scratch[FS_SECTOR_SIZE];
    memset(scratch, 0xA5, sizeof(scratch));
    uint32_t start = 0;
    if (fs_alloc_blocks(1, &start) != 0) return -1;
    const uint8_t* p = scratch;
    if (fs_journal_commit(&start, &p, 1) != 0) return -1;
    boot_sector.log_next = 2;
    d_write(0, &boot_sector);
    fs_recover();
    uint8_t verify[FS_SECTOR_SIZE];
    if (d_read(start, verify) != 0) return -1;
    for (int i = 0; i < FS_SECTOR_SIZE; i++) if (verify[i] != 0xA5) return -1;
    fs_free_blocks(start, 1);
    return fs_persist_meta();
}

/* ---- Extent helpers ---- */

static uint32_t extent_count_sane(uint32_t count) {
    /* Cap one extent so corrupt on-disk counts cannot explode walks. */
    uint32_t max = boot_sector.total_sectors ? boot_sector.total_sectors : 1024u;
    if (count > max) return 0;
    return count;
}

static uint32_t inode_sector_count(const struct fs_inode* in) {
    uint32_t n = 0;
    uint32_t max = boot_sector.total_sectors ? boot_sector.total_sectors : 1024u;
    for (int i = 0; i < FS_DIRECT_EXTENTS; i++) {
        uint32_t c = extent_count_sane(in->direct[i].count);
        if (n > max - c) return max;
        n += c;
    }
    if (in->indirect_lba) {
        struct fs_extent ind[FS_INDIRECT_MAX];
        memset(ind, 0, sizeof(ind));
        if (d_read(in->indirect_lba, ind) == 0) {
            for (uint32_t i = 0; i < FS_INDIRECT_MAX; i++) {
                uint32_t c = extent_count_sane(ind[i].count);
                if (n > max - c) return max;
                n += c;
            }
        }
    }
    return n;
}

static int inode_map_sector(const struct fs_inode* in, uint32_t file_sec, uint32_t* out_lba) {
    uint32_t left = file_sec;
    for (int i = 0; i < FS_DIRECT_EXTENTS; i++) {
        uint32_t c = extent_count_sane(in->direct[i].count);
        if (c && left < c) {
            uint32_t lba = in->direct[i].lba + left;
            if (boot_sector.total_sectors && lba >= boot_sector.total_sectors) return -1;
            *out_lba = lba;
            return 0;
        }
        left -= c;
    }
    if (!in->indirect_lba) return -1;
    struct fs_extent ind[FS_INDIRECT_MAX];
    memset(ind, 0, sizeof(ind));
    if (d_read(in->indirect_lba, ind) != 0) return -1;
    for (uint32_t i = 0; i < FS_INDIRECT_MAX; i++) {
        uint32_t c = extent_count_sane(ind[i].count);
        if (c && left < c) {
            uint32_t lba = ind[i].lba + left;
            if (boot_sector.total_sectors && lba >= boot_sector.total_sectors) return -1;
            *out_lba = lba;
            return 0;
        }
        left -= c;
    }
    return -1;
}

static void inode_clear_extents(struct fs_inode* in) {
    for (int i = 0; i < FS_DIRECT_EXTENTS; i++) {
        if (in->direct[i].count) fs_free_blocks(in->direct[i].lba, in->direct[i].count);
        in->direct[i].lba = in->direct[i].count = 0;
    }
    if (in->indirect_lba) {
        struct fs_extent ind[FS_INDIRECT_MAX];
        memset(ind, 0, sizeof(ind));
        if (d_read(in->indirect_lba, ind) == 0) {
            for (uint32_t i = 0; i < FS_INDIRECT_MAX; i++)
                if (ind[i].count) fs_free_blocks(ind[i].lba, ind[i].count);
        }
        fs_free_blocks(in->indirect_lba, 1);
        in->indirect_lba = 0;
    }
}

static int inode_add_extent(struct fs_inode* in, uint32_t lba, uint32_t count) {
    /* merge into last direct if contiguous */
    for (int i = 0; i < FS_DIRECT_EXTENTS; i++) {
        if (in->direct[i].count == 0) {
            in->direct[i].lba = lba;
            in->direct[i].count = count;
            return 0;
        }
        if (in->direct[i].lba + in->direct[i].count == lba) {
            in->direct[i].count += count;
            return 0;
        }
    }
    struct fs_extent ind[FS_INDIRECT_MAX];
    memset(ind, 0, sizeof(ind));
    if (!in->indirect_lba) {
        uint32_t ns = 0;
        if (fs_alloc_blocks(1, &ns) != 0) return -1;
        in->indirect_lba = ns;
    } else if (d_read(in->indirect_lba, ind) != 0) return -1;
    for (uint32_t i = 0; i < FS_INDIRECT_MAX; i++) {
        if (ind[i].count == 0) {
            ind[i].lba = lba;
            ind[i].count = count;
            return d_write(in->indirect_lba, ind);
        }
        if (ind[i].lba + ind[i].count == lba) {
            ind[i].count += count;
            return d_write(in->indirect_lba, ind);
        }
    }
    return -1;
}

static int inode_grow_sectors(struct fs_inode* in, uint32_t need_secs) {
    uint32_t have = inode_sector_count(in);
    while (have < need_secs) {
        uint32_t want = need_secs - have;
        if (want > 8) want = 8; /* allocate in chunks to allow fragmentation */
        uint32_t start = 0;
        /* try exact run; shrink until fits */
        while (want > 0 && fs_alloc_blocks(want, &start) != 0) want--;
        if (want == 0) return -1;
        if (inode_add_extent(in, start, want) != 0) {
            fs_free_blocks(start, want);
            return -1;
        }
        have += want;
    }
    return 0;
}

static int inode_shrink_to(struct fs_inode* in, uint32_t need_secs) {
    /* rebuild: copy kept data into new extents (AoW style for truncate) */
    uint32_t old_secs = inode_sector_count(in);
    if (need_secs >= old_secs) return 0;
    if (need_secs == 0) {
        inode_clear_extents(in);
        return 0;
    }
    uint8_t* buf = (uint8_t*)malloc(need_secs * FS_SECTOR_SIZE);
    if (!buf) return -1;
    memset(buf, 0, need_secs * FS_SECTOR_SIZE);
    for (uint32_t i = 0; i < need_secs; i++) {
        uint32_t lba = 0;
        if (inode_map_sector(in, i, &lba) != 0) {
            free(buf);
            return -1;
        }
        if (d_read(lba, buf + i * FS_SECTOR_SIZE) != 0) {
            free(buf);
            return -1;
        }
    }
    inode_clear_extents(in);
    /* ordered: write data first */
    uint32_t start = 0;
    if (fs_alloc_blocks(need_secs, &start) != 0) {
        if (inode_grow_sectors(in, need_secs) != 0) {
            free(buf);
            return -1;
        }
        for (uint32_t i = 0; i < need_secs; i++) {
            uint32_t lba = 0;
            if (inode_map_sector(in, i, &lba) != 0) {
                free(buf);
                return -1;
            }
            if (d_write(lba, buf + i * FS_SECTOR_SIZE) != 0) {
                free(buf);
                return -1;
            }
        }
        free(buf);
        return fs_cache_sync();
    }
    if (d_write_n(start, need_secs, buf) != 0) {
        fs_free_blocks(start, need_secs);
        free(buf);
        return -1;
    }
    free(buf);
    if (inode_add_extent(in, start, need_secs) != 0) {
        fs_free_blocks(start, need_secs);
        return -1;
    }
    return fs_cache_sync();
}

static uint32_t inode_byte_capacity(const struct fs_inode* in) {
    uint32_t secs = inode_sector_count(in);
    if (secs > (0xFFFFFFFFu / FS_SECTOR_SIZE)) return 0xFFFFFFFFu;
    return secs * FS_SECTOR_SIZE;
}

static uint32_t dir_logical_size(int dir_ino) {
    if (dir_ino <= 0 || dir_ino >= (int)FS_MAX_INODES) return 0;
    uint32_t size = inodes[dir_ino].size_bytes;
    uint32_t cap = inode_byte_capacity(&inodes[dir_ino]);
    if (size > cap) size = cap;
    /* Directory listings should stay small; avoid multi-MB reads on corrupt size. */
    if (size > 65536u) size = 65536u;
    return size;
}

static int dirent_next(uint32_t off, uint32_t size, const uint8_t* blob,
                       uint32_t* ino_out, uint8_t* nl_out, uint32_t* rec_out) {
    if (off + FS_DIRENT_HDR > size) return -1;
    uint32_t ino;
    memcpy(&ino, blob + off, 4);
    uint8_t nl = blob[off + 4];
    uint32_t rec = (FS_DIRENT_HDR + (uint32_t)nl + 3u) & ~3u;
    if (rec < FS_DIRENT_HDR) return -1;
    if (off + rec > size) return -1;
    if (nl && off + FS_DIRENT_HDR + nl > size) return -1;
    if (ino_out) *ino_out = ino;
    if (nl_out) *nl_out = nl;
    if (rec_out) *rec_out = rec;
    return 0;
}

static int dir_find_name(int dir_ino, const char* name, uint32_t* off_out) {
    if (dir_ino < 0 || !(inodes[dir_ino].flags & FS_FLAG_DIRECTORY)) return -1;
    uint32_t size = dir_logical_size(dir_ino);
    if (size == 0) return -1;
    uint8_t* blob = (uint8_t*)malloc(size);
    if (!blob) return -1;
    if (fs_index_read(dir_ino, 0, blob, size) < 0) {
        free(blob);
        return -1;
    }
    uint32_t off = 0;
    size_t nlen = 0;
    while (name[nlen]) nlen++;
    int found = -1;
    while (off < size) {
        uint32_t ino = 0, rec = 0;
        uint8_t nl = 0;
        if (dirent_next(off, size, blob, &ino, &nl, &rec) != 0) break;
        if (ino && nl == nlen && memcmp(blob + off + 5, name, nlen) == 0) {
            found = (int)ino;
            if (off_out) *off_out = off;
            break;
        }
        off += rec;
    }
    free(blob);
    return found;
}

static int dir_add_entry(int dir_ino, uint32_t child, const char* name) {
    size_t nlen = 0;
    while (name[nlen]) nlen++;
    if (nlen == 0 || nlen > FS_FILENAME_LEN) return -1;
    if (dir_find_name(dir_ino, name, 0) >= 0) return -1;
    uint32_t rec = FS_DIRENT_HDR + (uint32_t)nlen;
    rec = (rec + 3u) & ~3u;
    uint32_t old = dir_logical_size(dir_ino);
    uint8_t* blob = (uint8_t*)malloc(old + rec);
    if (!blob) return -1;
    memset(blob, 0, old + rec);
    if (old && fs_index_read(dir_ino, 0, blob, old) < 0) {
        free(blob);
        return -1;
    }
    memcpy(blob + old, &child, 4);
    blob[old + 4] = (uint8_t)nlen;
    memcpy(blob + old + 5, name, nlen);
    /* ordered: write dir data then meta */
    if (fs_index_truncate(dir_ino, old + rec) != 0) {
        free(blob);
        return -1;
    }
    int wr = fs_index_write(dir_ino, 0, blob, old + rec);
    free(blob);
    return wr == (int)(old + rec) ? 0 : -1;
}

static int dir_remove_entry(int dir_ino, const char* name) {
    uint32_t size = dir_logical_size(dir_ino);
    if (!size) return -1;
    uint8_t* blob = (uint8_t*)malloc(size);
    if (!blob) return -1;
    if (fs_index_read(dir_ino, 0, blob, size) < 0) {
        free(blob);
        return -1;
    }
    size_t nlen = 0;
    while (name[nlen]) nlen++;
    uint32_t off = 0;
    int ok = -1;
    while (off < size) {
        uint32_t ino = 0, rec = 0;
        uint8_t nl = 0;
        if (dirent_next(off, size, blob, &ino, &nl, &rec) != 0) break;
        if (ino && nl == nlen && memcmp(blob + off + 5, name, nlen) == 0) {
            memmove(blob + off, blob + off + rec, size - off - rec);
            size -= rec;
            ok = 0;
            break;
        }
        off += rec;
    }
    if (ok == 0) {
        if (fs_index_truncate(dir_ino, size) != 0) ok = -1;
        else if (size && fs_index_write(dir_ino, 0, blob, size) != (int)size) ok = -1;
    }
    free(blob);
    return ok;
}

static int dir_is_empty(int dir_ino) {
    uint32_t size = dir_logical_size(dir_ino);
    if (size == 0) return 1;
    uint8_t* blob = (uint8_t*)malloc(size);
    if (!blob) return 0;
    if (fs_index_read(dir_ino, 0, blob, size) < 0) {
        free(blob);
        return 0;
    }
    uint32_t off = 0;
    int empty = 1;
    while (off < size) {
        uint32_t ino = 0, rec = 0;
        uint8_t nl = 0;
        if (dirent_next(off, size, blob, &ino, &nl, &rec) != 0) break;
        if (ino) {
            empty = 0;
            break;
        }
        off += rec;
    }
    free(blob);
    return empty;
}

/* ---- Resolve ---- */

static int fs_follow_if_link(int idx, bool follow, int depth);

static int fs_resolve_component(const char* path, bool follow, int depth) {
    if (!path || !path[0]) return -1;
    if (path[0] == '/' && path[1] == 0) return (int)FS_ROOT_INO;

    char tmp[256];
    size_t n = 0;
    while (path[n] && n + 1 < sizeof(tmp)) {
        tmp[n] = path[n];
        n++;
    }
    tmp[n] = 0;

    int parent = (int)FS_ROOT_INO;
    const char* p = tmp;
    if (*p == '/') p++;
    char name[FS_FILENAME_LEN];
    int last = (int)FS_ROOT_INO;
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < FS_FILENAME_LEN - 1) name[i++] = *p++;
        name[i] = 0;
        if (*p == '/') p++;
        if (name[0] == 0) continue;
        if (name[0] == '.' && name[1] == 0) continue;
        if (name[0] == '.' && name[1] == '.' && name[2] == 0) {
            if (parent == (int)FS_ROOT_INO) continue;
            parent = (int)inodes[parent].parent;
            if (parent == 0) parent = (int)FS_ROOT_INO;
            last = parent;
            continue;
        }
        int idx = dir_find_name(parent, name, 0);
        if (idx < 0) return -1;
        bool more = (*p != 0);
        idx = fs_follow_if_link(idx, follow && (more || follow), depth);
        if (idx < 0) return -1;
        if (more) {
            if (!(inodes[idx].flags & FS_FLAG_DIRECTORY)) return -1;
            parent = idx;
        }
        last = idx;
    }
    return last;
}

static int fs_follow_if_link(int idx, bool follow, int depth) {
    if (idx < 0) return idx;
    if (!follow || !(inodes[idx].flags & FS_FLAG_SYMLINK)) return idx;
    if (depth >= FS_SYMLINK_MAX) return -1;
    char target[FS_FILENAME_LEN];
    if (inodes[idx].size_bytes == 0 || inodes[idx].size_bytes >= sizeof(target)) return -1;
    if (inodes[idx].size_bytes < FS_SYMLINK_INLINE && inodes[idx].symlink_inline[0]) {
        memcpy(target, inodes[idx].symlink_inline, inodes[idx].size_bytes);
    } else if (fs_index_read(idx, 0, target, inodes[idx].size_bytes) < 0) {
        return -1;
    }
    target[inodes[idx].size_bytes] = 0;
    return fs_resolve_component(target, true, depth + 1);
}

int fs_lookup_index(const char* path, bool follow_symlinks) {
    if (!fs_initialized || !path) return -1;
    return fs_resolve_component(path, follow_symlinks, 0);
}

bool fs_ready(void) { return fs_initialized; }

const struct fs_inode* fs_entry_at(int idx) {
    if (idx <= 0 || idx >= (int)FS_MAX_INODES) return 0;
    if (!(inodes[idx].flags & FS_FLAG_OCCUPIED)) return 0;
    return &inodes[idx];
}

int fs_access_ok(int idx, int want_write) {
    if (idx <= 0 || idx >= (int)FS_MAX_INODES) return -1;
    if (!(inodes[idx].flags & FS_FLAG_OCCUPIED)) return -1;
    if (g_uid == 0) return 0;
    uint16_t mode = inodes[idx].mode;
    if (inodes[idx].uid == g_uid) {
        if (want_write) return (mode & 0200) ? 0 : -1;
        return (mode & 0400) ? 0 : -1;
    }
    if (want_write) return (mode & 0020) ? 0 : -1;
    return (mode & 0004) ? 0 : -1;
}

int fs_index_stat(int idx, struct fs_stat* st) {
    if (!st || idx <= 0 || idx >= (int)FS_MAX_INODES) return -1;
    if (!(inodes[idx].flags & FS_FLAG_OCCUPIED)) return -1;
    st->size = inodes[idx].size_bytes;
    st->mode = inodes[idx].mode;
    st->mtime = inodes[idx].mtime;
    st->atime = inodes[idx].atime;
    st->ctime = inodes[idx].ctime;
    st->nlink = inodes[idx].nlink;
    st->uid = inodes[idx].uid;
    st->gid = inodes[idx].gid;
    st->flags = (uint8_t)inodes[idx].flags;
    st->start_sector = inodes[idx].direct[0].lba;
    st->sector_count = inode_sector_count(&inodes[idx]);
    return 0;
}

int fs_index_read(int idx, uint32_t offset, void* buffer, size_t size) {
    if (idx <= 0 || !buffer) return -1;
    if (!(inodes[idx].flags & FS_FLAG_OCCUPIED)) return -1;
    if (inodes[idx].flags & (FS_FLAG_FIFO | FS_FLAG_SOCK | FS_FLAG_BLK | FS_FLAG_CHR))
        return -1;
    if (fs_access_ok(idx, 0) != 0) return -1;
    if (inodes[idx].flags & FS_FLAG_DIRECTORY) {
        /* allow reading dir blob for internal helpers */
    } else {
        inodes[idx].atime = fs_now();
    }
    uint32_t logical = inodes[idx].size_bytes;
    uint32_t cap = inode_byte_capacity(&inodes[idx]);
    if (logical > cap) logical = cap;
    if (offset >= logical) return 0;
    size_t avail = logical - offset;
    if (size > avail) size = avail;
    if (size == 0) return 0;

    uint8_t* out = (uint8_t*)buffer;
    size_t done = 0;
    while (done < size) {
        uint32_t pos = offset + (uint32_t)done;
        uint32_t sec = pos / FS_SECTOR_SIZE;
        uint32_t off = pos % FS_SECTOR_SIZE;
        uint32_t lba = 0;
        if (inode_map_sector(&inodes[idx], sec, &lba) != 0) {
            /* sparse hole — but do not spin forever on corrupt metadata */
            size_t chunk = FS_SECTOR_SIZE - off;
            if (chunk > size - done) chunk = size - done;
            memset(out + done, 0, chunk);
            done += chunk;
            continue;
        }
        uint8_t tmp[FS_SECTOR_SIZE];
        if (d_read(lba, tmp) != 0) return -1;
        size_t chunk = FS_SECTOR_SIZE - off;
        if (chunk > size - done) chunk = size - done;
        memcpy(out + done, tmp + off, chunk);
        done += chunk;
    }
    return (int)done;
}

int fs_index_truncate(int idx, uint32_t new_size) {
    if (idx <= 0) return -1;
    if (!(inodes[idx].flags & FS_FLAG_OCCUPIED)) return -1;
    if (fs_access_ok(idx, 1) != 0) return -1;
    if (!bitmap && fs_load_bitmap() != 0) return -1;
    uint32_t need = new_size ? (new_size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE : 0;
    uint32_t have = inode_sector_count(&inodes[idx]);
    if (need > have) {
        if (inode_grow_sectors(&inodes[idx], need) != 0) return -1;
        /* zero new sectors already allocated blank from disk reuse — OK */
    } else if (need < have) {
        if (inode_shrink_to(&inodes[idx], need) != 0) return -1;
    }
    inodes[idx].size_bytes = new_size;
    inodes[idx].mtime = inodes[idx].ctime = fs_now();
    return fs_persist_meta();
}

int fs_index_write(int idx, uint32_t offset, const void* data, size_t size) {
    if (idx <= 0 || (!data && size)) return -1;
    if (!(inodes[idx].flags & FS_FLAG_OCCUPIED)) return -1;
    if (inodes[idx].flags & (FS_FLAG_FIFO | FS_FLAG_SOCK | FS_FLAG_BLK | FS_FLAG_CHR))
        return -1;
    if (fs_access_ok(idx, 1) != 0) return -1;
    uint32_t end = offset + (uint32_t)size;
    if (end < offset) return -1;
    if (end > inodes[idx].size_bytes) {
        if (fs_index_truncate(idx, end) != 0) return -1;
    }
    if (size == 0) return 0;

    /* ordered: write data sectors first, then meta */
    const uint8_t* src = (const uint8_t*)data;
    size_t done = 0;
    while (done < size) {
        uint32_t pos = offset + (uint32_t)done;
        uint32_t sec = pos / FS_SECTOR_SIZE;
        uint32_t off = pos % FS_SECTOR_SIZE;
        uint32_t lba = 0;
        if (inode_map_sector(&inodes[idx], sec, &lba) != 0) return -1;
        uint8_t tmp[FS_SECTOR_SIZE];
        if (d_read(lba, tmp) != 0) return -1;
        size_t chunk = FS_SECTOR_SIZE - off;
        if (chunk > size - done) chunk = size - done;
        memcpy(tmp + off, src + done, chunk);
        if (d_write(lba, tmp) != 0) return -1;
        done += chunk;
    }
    if (fs_cache_sync() != 0) return -1;
    inodes[idx].mtime = inodes[idx].ctime = fs_now();
    if (fs_persist_meta() != 0) return -1;
    return (int)size;
}

static int fs_alloc_inode(void) {
    for (int i = 1; i < (int)FS_MAX_INODES; i++)
        if (!(inodes[i].flags & FS_FLAG_OCCUPIED)) return i;
    return -1;
}

static int fs_parent_and_name(const char* path, int* parent_out, char* name_out) {
    if (!path || path[0] != '/') return -1;
    const char* slash = strrchr(path, '/');
    if (!slash) return -1;
    const char* base = slash + 1;
    if (!base[0]) return -1;
    strncpy(name_out, base, FS_FILENAME_LEN - 1);
    name_out[FS_FILENAME_LEN - 1] = 0;
    if (slash == path) {
        *parent_out = (int)FS_ROOT_INO;
        return 0;
    }
    char dir[256];
    size_t len = (size_t)(slash - path);
    if (len >= sizeof(dir)) return -1;
    memcpy(dir, path, len);
    dir[len] = 0;
    int didx = fs_lookup_index(dir, true);
    if (didx < 0 || !(inodes[didx].flags & FS_FLAG_DIRECTORY)) return -1;
    *parent_out = didx;
    return 0;
}

int fs_create_file(const char* path, uint16_t mode) {
    if (!fs_initialized) return -1;
    if (fs_lookup_index(path, false) >= 0) return -1;
    int parent;
    char name[FS_FILENAME_LEN];
    if (fs_parent_and_name(path, &parent, name) != 0) return -1;
    if (fs_access_ok(parent, 1) != 0) return -1;
    int idx = fs_alloc_inode();
    if (idx < 0) return -1;
    memset(&inodes[idx], 0, sizeof(inodes[idx]));
    inodes[idx].flags = FS_FLAG_OCCUPIED;
    inodes[idx].mode = mode ? mode : FS_MODE_FILE;
    inodes[idx].uid = g_uid;
    inodes[idx].gid = 0;
    inodes[idx].nlink = 1;
    inodes[idx].parent = (uint32_t)parent;
    inodes[idx].atime = inodes[idx].mtime = inodes[idx].ctime = fs_now();
    if (dir_add_entry(parent, (uint32_t)idx, name) != 0) {
        inodes[idx].flags = FS_FLAG_FREE;
        return -1;
    }
    boot_sector.inode_count++;
    if (fs_persist_meta() != 0) return -1;
    return idx;
}

static bool fs_layout_valid(void) {
    if (boot_sector.magic != FS_MAGIC) return false;
    if (boot_sector.version != FS_VERSION) return false;
    if (boot_sector.inode_table_sector == 0 || boot_sector.free_bitmap_sector == 0) return false;
    if (boot_sector.total_sectors < 64) return false;
    if (boot_sector.inode_slots == 0 || boot_sector.inode_slots > FS_MAX_INODES) return false;
    if (boot_sector.root_ino != FS_ROOT_INO) return false;
    uint32_t table_end = boot_sector.inode_table_sector + fs_inode_sectors();
    if (table_end >= boot_sector.total_sectors) return false;
    if (boot_sector.free_bitmap_sector < table_end) return false;
    uint32_t bitmap_secs = (boot_sector.free_bitmap_size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
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

int fs_fsck(bool repair) {
    if (!fs_initialized) return -1;
    int errors = 0;
    int repaired = 0;
    uint8_t* mark = (uint8_t*)malloc(boot_sector.total_sectors);
    if (!mark) return -1;
    memset(mark, 0, boot_sector.total_sectors);
    for (uint32_t i = 0; i < fs_alloc_base_sector(); i++) mark[i] = 1;

    uint32_t nlink_count[FS_MAX_INODES];
    memset(nlink_count, 0, sizeof(nlink_count));

    for (int i = 1; i < (int)FS_MAX_INODES; i++) {
        if (!(inodes[i].flags & FS_FLAG_OCCUPIED)) continue;
        uint32_t secs = inode_sector_count(&inodes[i]);
        for (uint32_t s = 0; s < secs; s++) {
            uint32_t lba = 0;
            if (inode_map_sector(&inodes[i], s, &lba) != 0) {
                errors++;
                continue;
            }
            if (lba >= boot_sector.total_sectors) {
                errors++;
                continue;
            }
            if (mark[lba]) errors++;
            mark[lba] = 1;
            if (!fs_bitmap_test(lba)) {
                errors++;
                if (repair) {
                    fs_bitmap_set(lba, 1);
                    repaired++;
                }
            }
        }
        if (inodes[i].indirect_lba) {
            if (inodes[i].indirect_lba < boot_sector.total_sectors)
                mark[inodes[i].indirect_lba] = 1;
        }
        if (inodes[i].flags & FS_FLAG_DIRECTORY) {
            uint32_t cap = inode_byte_capacity(&inodes[i]);
            if (inodes[i].size_bytes > cap) {
                errors++;
                if (repair) {
                    inodes[i].size_bytes = cap;
                    repaired++;
                }
            }
            uint32_t size = dir_logical_size(i);
            if (!size) continue;
            uint8_t* blob = (uint8_t*)malloc(size);
            if (!blob) continue;
            int dirty = 0;
            if (fs_index_read(i, 0, blob, size) >= 0) {
                uint32_t off = 0;
                while (off < size) {
                    uint32_t ino = 0, rec = 0;
                    uint8_t nl = 0;
                    if (dirent_next(off, size, blob, &ino, &nl, &rec) != 0) break;
                    if (ino) {
                        if (ino >= FS_MAX_INODES || !(inodes[ino].flags & FS_FLAG_OCCUPIED)) {
                            errors++;
                            if (repair) {
                                memset(blob + off, 0, 4);
                                dirty = 1;
                                repaired++;
                            }
                        } else nlink_count[ino]++;
                    }
                    off += rec;
                }
                if (repair && dirty) {
                    fs_index_write(i, 0, blob, size);
                }
            }
            free(blob);
        }
    }

    for (int i = 1; i < (int)FS_MAX_INODES; i++) {
        if (!(inodes[i].flags & FS_FLAG_OCCUPIED)) continue;
        if (i == (int)FS_ROOT_INO) continue;
        uint16_t expect = (uint16_t)nlink_count[i];
        if (expect == 0) {
            errors++;
            if (repair) {
                inode_clear_extents(&inodes[i]);
                memset(&inodes[i], 0, sizeof(inodes[i]));
                repaired++;
            }
        } else if (inodes[i].nlink != expect) {
            errors++;
            if (repair) {
                inodes[i].nlink = expect;
                repaired++;
            }
        }
    }

    /* rebuild free bits for unmarked data */
    if (repair) {
        for (uint32_t i = fs_alloc_base_sector(); i < boot_sector.total_sectors; i++) {
            if (!mark[i] && fs_bitmap_test(i)) {
                fs_bitmap_set(i, 0);
                repaired++;
            }
        }
        fs_persist_meta();
    }
    free(mark);
    if (errors == 0) log_msg(LOG_INFO, "fs", "fsck_ok");
    else if (repair && repaired) log_msg(LOG_INFO, "fs", "fsck_repaired");
    else if (errors) log_msg(LOG_ERR, "fs", "fsck_failed");
    return repair ? repaired : errors;
}

int fs_check_integrity(void) { return fs_fsck(false); }

int fs_init(int disk_id) {
    if (disk_id >= 0 && disk_select(disk_id) != 0) return -1;
    if (fs_initialized) return 0;
    fs_cache_init();

    log_msg(LOG_INFO, "fs", "init read superblock");
    if (disk_read_sector(0, &boot_sector) != 0) {
        log_msg(LOG_ERR, "fs", "superblock read fail");
        return -1;
    }

    bool need_create = !fs_layout_valid();
    if (need_create && boot_sector.magic == FS_MAGIC)
        log_msg(LOG_ERR, "fs", "old/invalid MOS layout — recreating");

    if (need_create) {
        log_msg(LOG_INFO, "fs", "create new filesystem v3");
        memset(&boot_sector, 0, sizeof(boot_sector));
        boot_sector.magic = FS_MAGIC;
        boot_sector.version = FS_VERSION;
        boot_sector.total_sectors = disk_get_size_sectors();
        if (boot_sector.total_sectors == 0) boot_sector.total_sectors = 1024;
        boot_sector.inode_table_sector = 1;
        boot_sector.inode_slots = FS_MAX_INODES;
        boot_sector.root_ino = FS_ROOT_INO;
        boot_sector.inode_count = 0;
        uint32_t table_sectors = fs_inode_sectors();
        boot_sector.free_bitmap_sector = 1 + table_sectors;
        boot_sector.free_bitmap_size = (boot_sector.total_sectors + 7) / 8;
        uint32_t bitmap_secs =
            (boot_sector.free_bitmap_size + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
        boot_sector.data_start_sector = boot_sector.free_bitmap_sector + bitmap_secs;
        uint32_t log_sectors = 64;
        boot_sector.log_start_sector = boot_sector.data_start_sector;
        boot_sector.log_size = log_sectors;
        boot_sector.log_next = 0;
        boot_sector.log_magic = 0x4C4F4700;

        memset(inodes, 0, sizeof(inodes));
        /* root inode */
        inodes[FS_ROOT_INO].flags = FS_FLAG_OCCUPIED | FS_FLAG_DIRECTORY;
        inodes[FS_ROOT_INO].mode = FS_MODE_DIR;
        inodes[FS_ROOT_INO].nlink = 1;
        inodes[FS_ROOT_INO].uid = 0;
        inodes[FS_ROOT_INO].parent = FS_ROOT_INO;
        inodes[FS_ROOT_INO].atime = inodes[FS_ROOT_INO].mtime = inodes[FS_ROOT_INO].ctime = fs_now();
        boot_sector.inode_count = 1;

        bitmap_sectors = bitmap_secs;
        bitmap = (uint8_t*)malloc(bitmap_sectors * FS_SECTOR_SIZE);
        if (!bitmap) return -1;
        memset(bitmap, 0, bitmap_sectors * FS_SECTOR_SIZE);
        for (uint32_t i = 0; i < boot_sector.data_start_sector + log_sectors; i++)
            fs_bitmap_set(i, 1);

        if (disk_write_sectors(boot_sector.free_bitmap_sector, bitmap_sectors, bitmap) != 0)
            return -1;
        if (disk_write_sector(0, &boot_sector) != 0) return -1;
        if (disk_write_sectors(boot_sector.inode_table_sector, table_sectors, inodes) != 0)
            return -1;

        fs_initialized = true;
        fs_cache_invalidate_all();
        fs_load_bitmap();
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
        log_msg(LOG_INFO, "fs", "init ok (new v3)");
        return 0;
    }

    log_msg(LOG_INFO, "fs", "load existing filesystem");
    if (disk_read_sectors(boot_sector.inode_table_sector, fs_inode_sectors(), inodes) != 0)
        return -1;
    fs_initialized = true;
    if (fs_load_bitmap() != 0) return -1;
    if (boot_sector.log_next > 0) fs_recover();
    /* Clamp corrupt directory sizes before fsck/list can hang on huge reads. */
    for (int i = 1; i < (int)FS_MAX_INODES; i++) {
        if (!(inodes[i].flags & FS_FLAG_OCCUPIED)) continue;
        uint32_t cap = inode_byte_capacity(&inodes[i]);
        if (inodes[i].size_bytes > cap) inodes[i].size_bytes = cap;
    }
    (void)fs_fsck(true);
    log_msg(LOG_INFO, "fs", "init ok");
    return 0;
}

int fs_open(const char* filename, uint32_t* size) {
    int idx = fs_lookup_index(filename, true);
    if (idx < 0) return -1;
    if (inodes[idx].flags & FS_FLAG_DIRECTORY) return -1;
    if (size) *size = inodes[idx].size_bytes;
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
    if (inodes[idx].flags & FS_FLAG_DIRECTORY) return -1;
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
    int dir = (int)FS_ROOT_INO;
    if (path && !(path[0] == '/' && path[1] == 0) && path[0]) {
        dir = fs_lookup_index(path, true);
        if (dir < 0 || !(inodes[dir].flags & FS_FLAG_DIRECTORY)) return -1;
    }
    uint32_t size = dir_logical_size(dir);
    size_t pos = 0;
    buffer[0] = 0;
    if (!size) return 0;

    uint8_t stackbuf[2048];
    uint8_t* blob = stackbuf;
    int heap = 0;
    if (size > sizeof(stackbuf)) {
        blob = (uint8_t*)malloc(size);
        if (!blob) return -1;
        heap = 1;
    }
    if (fs_index_read(dir, 0, blob, size) < 0) {
        if (heap) free(blob);
        return -1;
    }
    uint32_t off = 0;
    while (off < size && pos + 1 < buffer_size) {
        uint32_t ino = 0, rec = 0;
        uint8_t nl = 0;
        if (dirent_next(off, size, blob, &ino, &nl, &rec) != 0) break;
        if (ino && nl) {
            for (uint8_t k = 0; k < nl && pos + 1 < buffer_size; k++)
                buffer[pos++] = (char)blob[off + 5 + k];
            if (ino < FS_MAX_INODES && (inodes[ino].flags & FS_FLAG_DIRECTORY)) {
                if (pos + 1 < buffer_size) buffer[pos++] = '/';
            } else if (ino < FS_MAX_INODES && (inodes[ino].flags & FS_FLAG_SYMLINK)) {
                if (pos + 1 < buffer_size) buffer[pos++] = '@';
            }
            if (pos + 1 < buffer_size) buffer[pos++] = '\n';
        }
        off += rec;
    }
    buffer[pos] = 0;
    if (heap) free(blob);
    return (int)pos;
}

int fs_readdir(const char* path, uint32_t* offp, char* name_out, size_t name_cap, uint32_t* ino_out) {
    if (!offp || !name_out || name_cap == 0) return -1;
    int dir = (int)FS_ROOT_INO;
    if (path && !(path[0] == '/' && path[1] == 0)) {
        dir = fs_lookup_index(path, true);
        if (dir < 0 || !(inodes[dir].flags & FS_FLAG_DIRECTORY)) return -1;
    }
    uint32_t size = dir_logical_size(dir);
    if (*offp >= size) return 0;
    uint8_t stackbuf[2048];
    uint8_t* blob = stackbuf;
    int heap = 0;
    if (size > sizeof(stackbuf)) {
        blob = (uint8_t*)malloc(size);
        if (!blob) return -1;
        heap = 1;
    }
    if (fs_index_read(dir, 0, blob, size) < 0) {
        if (heap) free(blob);
        return -1;
    }
    uint32_t off = *offp;
    int got = 0;
    while (off < size) {
        uint32_t ino = 0, rec = 0;
        uint8_t nl = 0;
        uint32_t start = off;
        if (dirent_next(off, size, blob, &ino, &nl, &rec) != 0) break;
        off += rec;
        if (!ino || !nl) continue;
        size_t copy = nl;
        if (copy + 1 > name_cap) copy = name_cap - 1;
        memcpy(name_out, blob + start + 5, copy);
        name_out[copy] = 0;
        if (ino_out) *ino_out = ino;
        *offp = off;
        got = 1;
        break;
    }
    if (!got) *offp = size;
    if (heap) free(blob);
    return got;
}

int fs_delete(const char* filename) {
    if (!fs_initialized) return -1;
    int idx = fs_lookup_index(filename, false);
    if (idx < 0 || idx == (int)FS_ROOT_INO) return -1;
    if (fs_access_ok(idx, 1) != 0) return -1;
    if ((inodes[idx].flags & FS_FLAG_DIRECTORY) && !dir_is_empty(idx)) return -1;

    int parent;
    char name[FS_FILENAME_LEN];
    if (fs_parent_and_name(filename, &parent, name) != 0) return -1;
    if (dir_remove_entry(parent, name) != 0) return -1;

    if (inodes[idx].nlink > 1) {
        inodes[idx].nlink--;
        inodes[idx].ctime = fs_now();
    } else {
        inode_clear_extents(&inodes[idx]);
        if (inodes[idx].xattr_lba) fs_free_blocks(inodes[idx].xattr_lba, 1);
        memset(&inodes[idx], 0, sizeof(inodes[idx]));
        if (boot_sector.inode_count) boot_sector.inode_count--;
    }
    return fs_persist_meta();
}

int fs_link(const char* oldpath, const char* newpath) {
    if (!fs_initialized) return -1;
    int idx = fs_lookup_index(oldpath, false);
    if (idx < 0) return -1;
    if (inodes[idx].flags & FS_FLAG_DIRECTORY) return -1;
    if (fs_lookup_index(newpath, false) >= 0) return -1;
    int parent;
    char name[FS_FILENAME_LEN];
    if (fs_parent_and_name(newpath, &parent, name) != 0) return -1;
    if (dir_add_entry(parent, (uint32_t)idx, name) != 0) return -1;
    inodes[idx].nlink++;
    inodes[idx].ctime = fs_now();
    return fs_persist_meta();
}

int fs_rename(const char* old_path, const char* new_path) {
    if (!fs_initialized) return -1;
    int idx = fs_lookup_index(old_path, false);
    if (idx < 0) return -1;
    if (fs_lookup_index(new_path, false) >= 0) return -1;
    int old_parent, new_parent;
    char old_name[FS_FILENAME_LEN], new_name[FS_FILENAME_LEN];
    if (fs_parent_and_name(old_path, &old_parent, old_name) != 0) return -1;
    if (fs_parent_and_name(new_path, &new_parent, new_name) != 0) return -1;
    if (dir_add_entry(new_parent, (uint32_t)idx, new_name) != 0) return -1;
    if (dir_remove_entry(old_parent, old_name) != 0) return -1;
    inodes[idx].parent = (uint32_t)new_parent;
    inodes[idx].ctime = fs_now();
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
    if (g_uid != 0 && inodes[idx].uid != g_uid) return -1;
    inodes[idx].mode = mode;
    inodes[idx].ctime = fs_now();
    return fs_persist_meta();
}

int fs_chown(const char* path, uint16_t uid, uint16_t gid) {
    int idx = fs_lookup_index(path, false);
    if (idx < 0) return -1;
    if (g_uid != 0) return -1;
    inodes[idx].uid = uid;
    inodes[idx].gid = gid;
    inodes[idx].ctime = fs_now();
    return fs_persist_meta();
}

int fs_touch(const char* path) {
    int idx = fs_lookup_index(path, false);
    if (idx < 0) {
        idx = fs_create_file(path, FS_MODE_FILE);
        if (idx < 0) return -1;
        return 0;
    }
    inodes[idx].mtime = inodes[idx].atime = fs_now();
    return fs_persist_meta();
}

int fs_symlink(const char* target, const char* linkpath) {
    if (!target || !linkpath) return -1;
    if (fs_lookup_index(linkpath, false) >= 0) return -1;
    int idx = fs_create_file(linkpath, FS_MODE_LINK);
    if (idx < 0) return -1;
    inodes[idx].flags = FS_FLAG_OCCUPIED | FS_FLAG_SYMLINK;
    size_t len = 0;
    while (target[len]) len++;
    if (len < FS_SYMLINK_INLINE) {
        memcpy(inodes[idx].symlink_inline, target, len);
        inodes[idx].symlink_inline[len] = 0;
        inodes[idx].size_bytes = (uint32_t)len;
        return fs_persist_meta();
    }
    if (fs_index_truncate(idx, (uint32_t)len) != 0) return -1;
    if (len && fs_index_write(idx, 0, target, len) != (int)len) return -1;
    return 0;
}

int fs_readlink(const char* path, char* buf, size_t buf_size) {
    int idx = fs_lookup_index(path, false);
    if (idx < 0 || !(inodes[idx].flags & FS_FLAG_SYMLINK)) return -1;
    if (!buf || buf_size == 0) return -1;
    size_t n = inodes[idx].size_bytes;
    if (n + 1 > buf_size) n = buf_size - 1;
    if (n < FS_SYMLINK_INLINE && inodes[idx].symlink_inline[0]) {
        memcpy(buf, inodes[idx].symlink_inline, n);
        buf[n] = 0;
        return (int)n;
    }
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

    int parent = (int)FS_ROOT_INO;
    char* p = copy;
    if (*p == '/') p++;
    while (*p) {
        char* slash = p;
        while (*slash && *slash != '/') slash++;
        char save = *slash;
        *slash = 0;
        if (p[0]) {
            int idx = dir_find_name(parent, p, 0);
            if (idx < 0) {
                idx = fs_alloc_inode();
                if (idx < 0) return -1;
                memset(&inodes[idx], 0, sizeof(inodes[idx]));
                inodes[idx].flags = FS_FLAG_OCCUPIED | FS_FLAG_DIRECTORY;
                inodes[idx].mode = FS_MODE_DIR;
                inodes[idx].nlink = 1;
                inodes[idx].uid = g_uid;
                inodes[idx].parent = (uint32_t)parent;
                inodes[idx].atime = inodes[idx].mtime = inodes[idx].ctime = fs_now();
                if (dir_add_entry(parent, (uint32_t)idx, p) != 0) {
                    inodes[idx].flags = FS_FLAG_FREE;
                    return -1;
                }
                boot_sector.inode_count++;
                if (fs_persist_meta() != 0) return -1;
                parent = idx;
            } else {
                if (!(inodes[idx].flags & FS_FLAG_DIRECTORY)) return -1;
                parent = idx;
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

/* Minimal xattr: one sector, records name\0value\0 ... */
int fs_setxattr(const char* path, const char* name, const void* val, size_t size) {
    int idx = fs_lookup_index(path, false);
    if (idx < 0 || !name) return -1;
    if (!inodes[idx].xattr_lba) {
        uint32_t s = 0;
        if (fs_alloc_blocks(1, &s) != 0) return -1;
        inodes[idx].xattr_lba = s;
        uint8_t z[FS_SECTOR_SIZE];
        memset(z, 0, sizeof(z));
        d_write(s, z);
    }
    uint8_t sec[FS_SECTOR_SIZE];
    if (d_read(inodes[idx].xattr_lba, sec) != 0) return -1;
    /* simplistic: clear and write single attr */
    memset(sec, 0, sizeof(sec));
    size_t nl = 0;
    while (name[nl]) nl++;
    if (nl + 1 + size + 1 >= FS_SECTOR_SIZE) return -1;
    memcpy(sec, name, nl);
    sec[nl] = 0;
    memcpy(sec + nl + 1, val, size);
    sec[nl + 1 + size] = 0;
    if (d_write(inodes[idx].xattr_lba, sec) != 0) return -1;
    return fs_persist_meta();
}

int fs_getxattr(const char* path, const char* name, void* buf, size_t size) {
    int idx = fs_lookup_index(path, false);
    if (idx < 0 || !name || !inodes[idx].xattr_lba) return -1;
    uint8_t sec[FS_SECTOR_SIZE];
    if (d_read(inodes[idx].xattr_lba, sec) != 0) return -1;
    size_t nl = 0;
    while (name[nl]) nl++;
    if (memcmp(sec, name, nl) != 0 || sec[nl] != 0) return -1;
    const uint8_t* v = sec + nl + 1;
    size_t vl = 0;
    while (v[vl] && nl + 1 + vl < FS_SECTOR_SIZE) vl++;
    if (vl > size) vl = size;
    if (buf) memcpy(buf, v, vl);
    return (int)vl;
}
